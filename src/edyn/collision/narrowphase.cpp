#include "edyn/collision/narrowphase.hpp"
#include "edyn/comp/constraint.hpp"
#include "edyn/comp/material.hpp"
#include "edyn/comp/position.hpp"
#include "edyn/comp/orientation.hpp"
#include "edyn/comp/shape.hpp"
#include "edyn/comp/constraint_row.hpp"
#include "edyn/comp/tag.hpp"
#include "edyn/comp/aabb.hpp"
#include "edyn/comp/island.hpp"
#include "edyn/collision/contact_manifold.hpp"
#include "edyn/collision/contact_point.hpp"
#include "edyn/collision/collide.hpp"
#include "edyn/math/geom.hpp"
#include "edyn/util/constraint_util.hpp"
#include "edyn/parallel/parallel_for_async.hpp"
#include <entt/entt.hpp>

namespace edyn {

// Update distance of persisted contact points.
static
void update_contact_distances(entt::registry &registry) {
    auto cp_view = registry.view<contact_point>();
    auto tr_view = registry.view<position, orientation>();

    cp_view.each([&] (auto, contact_point &cp) {
        auto [posA, ornA] = tr_view.get<position, orientation>(cp.body[0]);
        auto [posB, ornB] = tr_view.get<position, orientation>(cp.body[1]);
        auto pivotA_world = posA + rotate(ornA, cp.pivotA);
        auto pivotB_world = posB + rotate(ornB, cp.pivotB);
        auto normal_world = rotate(ornB, cp.normalB);
        cp.distance = dot(normal_world, pivotA_world - pivotB_world);
    });
}

// Merges a `collision_point` onto a `contact_point`.
static
void merge_point(const collision_result::collision_point &rp, contact_point &cp) {
    cp.pivotA = rp.pivotA;
    cp.pivotB = rp.pivotB;
    cp.normalB = rp.normalB;
    cp.distance = rp.distance;
}

static
void create_contact_constraint(entt::registry &registry, entt::entity manifold_entity,
                               entt::entity contact_entity, contact_point &cp) {
    auto &materialA = registry.get<material>(cp.body[0]);
    auto &materialB = registry.get<material>(cp.body[1]);

    cp.restitution = materialA.restitution * materialB.restitution;
    cp.friction = materialA.friction * materialB.friction;

    auto stiffness = large_scalar;
    auto damping = large_scalar;

    if (materialA.stiffness < large_scalar || materialB.stiffness < large_scalar) {
        stiffness = 1 / (1 / materialA.stiffness + 1 / materialB.stiffness);
        damping = 1 / (1 / materialA.damping + 1 / materialB.damping);
    }

    auto contact = contact_constraint();
    contact.stiffness = stiffness;
    contact.damping = damping;

    make_constraint(contact_entity, registry, std::move(contact), cp.body[0], cp.body[1], &manifold_entity);
}

template<typename ContactPointViewType> static 
size_t find_nearest_contact(const contact_manifold &manifold, 
                            const collision_result::collision_point &coll_pt,
                            const ContactPointViewType &cp_view) {
    auto shortest_dist = contact_caching_threshold * contact_caching_threshold;
    auto nearest_idx = manifold.num_points();

    for (size_t i = 0; i < manifold.num_points(); ++i) {
        auto &cp = cp_view.template get<contact_point>(manifold.point[i]);
        auto dA = length_sqr(coll_pt.pivotA - cp.pivotA);
        auto dB = length_sqr(coll_pt.pivotB - cp.pivotB);

        if (dA < shortest_dist) {
            shortest_dist = dA;
            nearest_idx = i;
        }

        if (dB < shortest_dist) {
            shortest_dist = dB;
            nearest_idx = i;
        }
    }

    return nearest_idx;
}

static
void create_contact_point(entt::registry &registry, entt::entity manifold_entity,
                          contact_manifold &manifold, const collision_result::collision_point &rp) {
    auto idx = manifold.num_points();
    if (idx >= max_contacts) return;
    
    auto contact_entity = registry.create();
    manifold.point[idx] = contact_entity;

    auto &cp = registry.emplace<contact_point>(
        contact_entity, 
        manifold.body,
        rp.pivotA, // pivotA
        rp.pivotB, // pivotB
        rp.normalB, // normalB
        0, // friction
        0, // restitution
        0, // lifetime
        rp.distance // distance
    );

    if (registry.has<material>(manifold.body[0]) && registry.has<material>(manifold.body[1])) {
        create_contact_constraint(registry, manifold_entity, contact_entity, cp);
    }

    registry.get_or_emplace<dirty>(contact_entity)
        .set_new()
        .created<contact_point>();

    registry.get_or_emplace<dirty>(manifold_entity).updated<contact_manifold>();
}

static
void destroy_contact_point(entt::registry &registry, entt::entity manifold_entity, entt::entity contact_entity) {
    registry.destroy(contact_entity);

    auto &node_parent = registry.get<island_node_parent>(manifold_entity);
    node_parent.children.erase(contact_entity);

    registry.get_or_emplace<dirty>(manifold_entity)
        .updated<contact_manifold, island_node_parent>();
}

using contact_point_view_t = entt::basic_view<entt::entity, entt::exclude_t<>, contact_point, constraint>; 
using constraint_row_view_t = entt::basic_view<entt::entity, entt::exclude_t<>, constraint_row_data>; 

template<typename Function> static
void process_collision(entt::entity manifold_entity, contact_manifold &manifold, 
                       const collision_result &result,
                       const contact_point_view_t &cp_view, 
                       const constraint_row_view_t &cr_view, Function new_point_func) {
    // Merge new with existing contact points.
    for (size_t i = 0; i < result.num_points; ++i) {
        auto &rp = result.point[i];

        // Find closest existing point.
        auto nearest_idx = find_nearest_contact(manifold, rp, cp_view);

        if (nearest_idx < manifold.num_points()) {
            auto &cp = cp_view.get<contact_point>(manifold.point[nearest_idx]);
            ++cp.lifetime;
            merge_point(rp, cp);
        } else {
            // Assign it to array of points and set it up.
            // Find best insertion index. Try pivotA first.
            std::array<vector3, max_contacts> pivots;
            std::array<scalar, max_contacts> distances;
            for (size_t i = 0; i < manifold.num_points(); ++i) {
                auto &cp = cp_view.get<contact_point>(manifold.point[i]);
                pivots[i] = cp.pivotB;
                distances[i] = cp.distance;
            }

            auto idx = insert_index(pivots, distances, manifold.num_points(), rp.pivotB, rp.distance);

            // No closest point found for pivotA, try pivotB.
            if (idx >= manifold.num_points()) {
                for (size_t i = 0; i < manifold.num_points(); ++i) {
                    auto &cp = cp_view.get<contact_point>(manifold.point[i]);
                    pivots[i] = cp.pivotB;
                }

                idx = insert_index(pivots, distances, manifold.num_points(), rp.pivotB, rp.distance);
            }

            if (idx < max_contacts) {
                auto is_new_contact = idx == manifold.num_points();

                if (is_new_contact) {
                    new_point_func(rp);
                } else {
                    // Replace existing contact point.
                    auto contact_entity = manifold.point[idx];
                    auto &cp = cp_view.get<contact_point>(contact_entity);
                    cp.lifetime = 0;
                    merge_point(rp, cp);

                    // Zero out warm-starting impulses.
                    auto &con = cp_view.get<constraint>(contact_entity);
                    for (size_t i = 0; i < con.num_rows(); ++i) {
                        cr_view.get(con.row[i]).impulse = 0;
                    }
                }
            }
        }
    }
}

template<typename ContactPointViewType, typename Function> static
void prune(contact_manifold &manifold,
           const vector3 &posA, const quaternion &ornA, 
           const vector3 &posB, const quaternion &ornB,
           const ContactPointViewType &cp_view, Function destroy_point_func) {
    constexpr auto threshold_sqr = contact_breaking_threshold * contact_breaking_threshold;

    // Remove separating contact points.
    for (size_t i = manifold.num_points(); i > 0; --i) {
        size_t k = i - 1;
        auto point_entity = manifold.point[k];
        auto &cp = cp_view.template get<contact_point>(point_entity);
        auto pA = posA + rotate(ornA, cp.pivotA);
        auto pB = posB + rotate(ornB, cp.pivotB);
        auto n = rotate(ornB, cp.normalB);
        auto d = pA - pB;
        auto dn = dot(d, n); // separation along normal
        auto dp = d - dn * n; // tangential separation on contact plane

        if (dn > contact_breaking_threshold || length_sqr(dp) > threshold_sqr) {
            // Swap with last element.
            size_t last_idx = manifold.num_points() - 1;
            
            if (last_idx != k) {
                manifold.point[k] = manifold.point[last_idx];
            }

            manifold.point[last_idx] = entt::null;

            destroy_point_func(point_entity);
        }
    }
}

void detect_collision(const contact_manifold &manifold, collision_result &result, 
                      const body_view_t &body_view) {
    auto [aabbA, posA, ornA] = body_view.get<AABB, position, orientation>(manifold.body[0]);
    auto [aabbB, posB, ornB] = body_view.get<AABB, position, orientation>(manifold.body[1]);
    const auto offset = vector3_one * -contact_breaking_threshold;

    // Only proceed to closest points calculation if AABBs intersect, since
    // a manifold is allowed to exist whilst the AABB separation is smaller
    // than `manifold.separation_threshold` which is greater than the
    // contact breaking threshold.
    if (intersect(aabbA.inset(offset), aabbB)) {
        auto &shapeA = body_view.get<shape>(manifold.body[0]);
        auto &shapeB = body_view.get<shape>(manifold.body[1]);

        // Structured binding is not captured by lambda, thus use an explicit
        // capture list (https://stackoverflow.com/a/48103632/749818).
        std::visit([&result, pA = posA, oA = ornA, pB = posB, oB = ornB] (auto &&sA, auto &&sB) {
            result = collide(sA, pA, oA, sB, pB, oB, 
                             contact_breaking_threshold);
        }, shapeA.var, shapeB.var);
    } else {
        result.num_points = 0;
    }
}

void process_result(entt::registry &registry, entt::entity manifold_entity, 
                    contact_manifold &manifold, const collision_result &result, 
                    const transform_view_t &tr_view) {
    auto cp_view = registry.view<contact_point, constraint>();
    auto cr_view = registry.view<constraint_row_data>();
    process_collision(manifold_entity, manifold, result, cp_view, cr_view,
                      [&] (const collision_result::collision_point &rp) {
        create_contact_point(registry, manifold_entity, manifold, rp);
    });

    auto [posA, ornA] = tr_view.get<position, orientation>(manifold.body[0]);
    auto [posB, ornB] = tr_view.get<position, orientation>(manifold.body[1]);
    prune(manifold, posA, ornA, posB, ornB, cp_view,
          [&] (entt::entity contact_entity) {
        destroy_contact_point(registry, manifold_entity, contact_entity);
    });
}

narrowphase::narrowphase(entt::registry &reg)
    : m_registry(&reg)
{}

bool narrowphase::parallelizable() const {
    return m_registry->size<contact_manifold>() > 1;
}

void narrowphase::update() {
    update_contact_distances(*m_registry);

    auto manifold_view = m_registry->view<contact_manifold>();
    update_contact_manifolds(manifold_view.begin(), manifold_view.end(), manifold_view);
}

void narrowphase::update_async(job &completion_job) {
    update_contact_distances(*m_registry);

    EDYN_ASSERT(parallelizable());

    auto manifold_view = m_registry->view<contact_manifold>();
    auto body_view = m_registry->view<AABB, shape, position, orientation>();
    auto cp_view = m_registry->view<contact_point, constraint>();
    auto cr_view = m_registry->view<constraint_row_data>();

    // Resize result collection vectors to allocate one slot for each iteration
    // of the parallel_for.
    m_cp_construction_infos.resize(manifold_view.size());
    m_cp_destruction_infos.resize(manifold_view.size());
    auto &dispatcher = job_dispatcher::global();

    parallel_for_async(dispatcher, size_t{0}, manifold_view.size(), size_t{1}, completion_job, 
            [this, body_view, manifold_view, cp_view, cr_view] (size_t index) {
        auto entity = manifold_view[index];
        auto &manifold = manifold_view.get(entity);
        collision_result result;
        auto &construction_info = m_cp_construction_infos[index];

        detect_collision(manifold, result, body_view);
        process_collision(entity, manifold, result, cp_view, cr_view,
                          [&construction_info] (const collision_result::collision_point &rp) {
            construction_info.point[construction_info.count++] = rp;
        });
        
        auto &destruction_info = m_cp_destruction_infos[index];
        auto [posA, ornA] = body_view.get<position, orientation>(manifold.body[0]);
        auto [posB, ornB] = body_view.get<position, orientation>(manifold.body[1]);

        prune(manifold, posA, ornA, posB, ornB, cp_view, 
              [&destruction_info] (entt::entity contact_entity) {
            destruction_info.contact_entity[destruction_info.count++] = contact_entity;
        });
    });
}

void narrowphase::finish_async_update() {
    auto manifold_view = m_registry->view<contact_manifold>();

    // Create contact points.
    for (size_t i = 0; i < manifold_view.size(); ++i) {
        auto entity = manifold_view[i];
        auto &manifold = manifold_view.get(entity);
        auto &info_result = m_cp_construction_infos[i];

        for (size_t j = 0; j < info_result.count; ++j) {
            create_contact_point(*m_registry, entity, manifold, info_result.point[j]);
        }
    }

    m_cp_construction_infos.clear();

    // Destroy contact points.
    for (size_t i = 0; i < manifold_view.size(); ++i) {
        auto entity = manifold_view[i];
        auto &info_result = m_cp_destruction_infos[i];

        for (size_t j = 0; j < info_result.count; ++j) {
            destroy_contact_point(*m_registry, entity, info_result.contact_entity[j]);
        }
    }

    m_cp_destruction_infos.clear();
}

}