#ifndef EDYN_PARALLEL_ISLAND_DELTA_BUILDER_HPP
#define EDYN_PARALLEL_ISLAND_DELTA_BUILDER_HPP

#include <tuple>
#include <memory>
#include <utility>
#include "edyn/parallel/island_delta.hpp"
#include "edyn/parallel/entity_component_map.hpp"

namespace edyn {

/**
 * @brief Provides the means to build a `island_delta`.
 */
class island_delta_builder {
    using map_of_component_map = std::unordered_map<entt::id_type, std::unique_ptr<entity_component_map_base>>;

    template<typename Component>
    void _created(entt::entity entity, const Component &component) {
        auto id = entt::type_index<Component>::value();
        
        if (m_created_components.count(id) == 0) {
            auto map = std::make_unique<entity_component_map<Component>>();
            map->insert(entity, component);
            m_created_components.insert({id, std::move(map)});
            m_delta.prepare_created<Component>();
        } else {
            auto &map = static_cast<entity_component_map<Component> &>(*m_created_components.at(id));

            if (map.empty()) {
                // Empty means this was cleared before and needs to be initialized
                // again in the current delta.
                m_delta.prepare_created<Component>();
            }

            map.insert(entity, component);
        }
    }

    template<typename Component>
    void _updated(entt::entity entity, const Component &component) {
        auto id = entt::type_index<Component>::value();

        if (m_updated_components.count(id) == 0) {
            auto map = std::make_unique<entity_component_map<Component>>();
            map->insert(entity, component);
            m_updated_components.insert({id, std::move(map)});
            m_delta.prepare_updated<Component>();
        } else {
            auto &map = static_cast<entity_component_map<Component> &>(*m_updated_components.at(id));

            if (map.empty()) {
                m_delta.prepare_updated<Component>();
            }

            map.insert(entity, component);
        }
    }

    template<typename Component>
    void _destroyed(entt::entity entity) {
        auto id = entt::type_index<Component>::value();

        if (m_destroyed_components.count(id) == 0) {
            auto set = std::make_unique<entity_component_set<Component>>();
            set->insert(entity);
            m_destroyed_components.insert({id, std::move(set)});
            m_delta.prepare_destroyed<Component>();
        } else {
            auto &set = static_cast<entity_component_set<Component> &>(*m_destroyed_components.at(id));

            if (set.empty()) {
                m_delta.prepare_destroyed<Component>();
            }

            set.insert(entity);
        }
    }

public:
    island_delta_builder(entity_map &map)
        : m_entity_map(&map)
    {}

    virtual ~island_delta_builder() {}

    /**
     * @brief Inserts a mapping into the current delta for a local entity.
     * Assumes a mapping exists in the entity map.
     * @param entity An entity in the local registry.
     */
    void insert_entity_mapping(entt::entity);

    /**
     * @brief Marks the given entity as newly created.
     * @param entity The newly created entity.
     */
    void created(entt::entity);

    /**
     * @brief Adds the given components to the list of newly created components.
     * @tparam Component Pack of component types.
     * @param entity The entity the components have been assigned to.
     * @param comp A pack of component instances.
     */
    template<typename... Component>
    void created(entt::entity entity, const Component &... comp) {
        (_created(entity, comp), ...);
    }

    /**
     * @brief Fetches the requested components from the given registry and adds
     * them to the list of newly created components.
     * @tparam Component Pack of component types.
     * @param entity The entity the components have been assigned to.
     * @param registry The source registry.
     */
    template<typename Component>
    void created(entt::entity entity, entt::registry &registry) {
        if constexpr(entt::is_eto_eligible_v<Component>) {
            created(entity, Component{});
        } else {
            created(entity, registry.get<Component>(entity));
        }
    }

    /**
     * @brief Fetches the requested component by id (i.e. `entt::id_type`) from
     * the given registry and adds it to the list of newly created components.
     * @param entity The entity the component has been assigned to.
     * @param registry The source registry.
     * @param id A component id obtained using `entt::type_index`.
     */
    virtual void created(entt::entity entity, entt::registry &registry, entt::id_type id) = 0;

    /**
     * @brief Fetches the requested components by id (i.e. `entt::id_type`) from
     * the given registry and adds them to the list of newly created components.
     * @tparam It Type of input iterator.
     * @param entity The entity the components have been assigned to.
     * @param registry The source registry.
     * @param first An iterator to the first element of a range of component ids.
     * @param last An iterator past the last element of a range of component ids.
     */
    template<typename It>
    void created(entt::entity entity, entt::registry &registry, It first, It last) {
        for (auto it = first; it != last; ++it) {
            created(entity, registry, *it);
        }
    }

    /**
     * @brief Marks all registered component types that the given entity has as
     * newly created. Useful to be called for entities that have just been
     * constructed.
     * @param entity The entity the components have been assigned to.
     * @param registry The source registry.
     */
    virtual void created_all(entt::entity entity, entt::registry &registry) = 0;

    /**
     * @brief Adds the given components to the list of updated components.
     * @tparam Component Pack of component types.
     * @param entity The entity that owns the given components.
     * @param comp A pack of component instances.
     */
    template<typename... Component>
    void updated(entt::entity entity, const Component &... comp) {
        (_updated(entity, comp), ...);
    }

    /**
     * @brief Fetches the requested components from the given registry and adds
     * them to the list of updated components.
     * @tparam Component Pack of component types.
     * @param entity The entity that owns the given components.
     * @param registry The source registry.
     */
    template<typename... Component>
    void updated(entt::entity entity, entt::registry &registry) {
        if constexpr(sizeof...(Component) == 1) {
            if constexpr(std::conjunction_v<entt::is_eto_eligible<Component>...>) {
                (updated(entity, Component{}), ...);
            } else {
                (updated<Component>(entity, registry.get<Component>(entity)), ...);
            }
        } else {
            (updated<Component>(entity, registry), ...);
        }
    }

    /**
     * @brief Fetches the requested component by id (i.e. `entt::id_type`) from
     * the given registry and adds it to the list of updated components.
     * @tparam It Type of input iterator.
     * @param entity The entity the component has been assigned to.
     * @param registry The source registry.
     * @param id A component id obtained using `entt::type_index`.
     */
    virtual void updated(entt::entity entity, entt::registry &registry, entt::id_type id) = 0;

    /**
     * @brief Fetches the requested components by id (i.e. `entt::id_type`) from
     * the given registry and adds them to the list of updated components.
     * @tparam It Type of input iterator.
     * @param entity The entity the components have been assigned to.
     * @param registry The source registry.
     * @param first An iterator to the first element of a range of component ids.
     * @param last An iterator past the last element of a range of component ids.
     */
    template<typename It>
    void updated(entt::entity entity, entt::registry &registry, It first, It last) {
        for (auto it = first; it != last; ++it) {
            updated(entity, registry, *it);
        }
    }

    /**
     * @brief Marks all registered component types that the given entity has as
     * update.
     * @param entity The entity that owns the components.
     * @param registry The source registry.
     */
    virtual void updated_all(entt::entity entity, entt::registry &registry) = 0;

    /**
     * @brief Marks components as deleted or marks the entity as destroyed if no
     * component is specified.
     * @tparam Component Pack of component types.
     * @param entity The entity that owns the given components.
     */
    template<typename... Component>
    void destroyed(entt::entity entity) {
        if constexpr(sizeof...(Component) == 0) {
            m_delta.m_destroyed_entities.push_back(entity);
        } else {
            (_destroyed<Component>(entity), ...);
        }
    }

    /**
     * @brief Marks components as deleted by id (i.e. `entt::id_type`).
     * @param entity The entity that owned the deleted components.
     * @param id A component id obtained using `entt::type_index`.
     */
    virtual void destroyed(entt::entity entity, entt::id_type id) = 0;

    /**
     * @brief Marks components as deleted by id (i.e. `entt::id_type`).
     * @tparam It Type of input iterator.
     * @param entity The entity that owned the deleted components.
     * @param first An iterator to the first element of a range of component ids.
     * @param last An iterator past the last element of a range of component ids.
     */
    template<typename It>
    void destroyed(entt::entity entity, It first, It last) {
        for (auto it = first; it != last; ++it) {
            destroyed(entity, *it);
        }
    }

    bool empty() const;

    island_delta finish() {
        // Load components into delta.
        for (auto &pair : m_delta.m_created_components) {
            pair.second->load(*m_created_components.at(pair.first));
        }

        for (auto &pair : m_delta.m_updated_components) {
            pair.second->load(*m_updated_components.at(pair.first));
        }

        for (auto &pair : m_delta.m_destroyed_components) {
            pair.second->load(*m_destroyed_components.at(pair.first));
        }

        // Clear containers of local components.
        for (auto &pair : m_created_components) {
            pair.second->clear();
        }

        for (auto &pair : m_updated_components) {
            pair.second->clear();
        }

        for (auto &pair : m_destroyed_components) {
            pair.second->clear();
        }

        // Move the contents of `m_delta` into the returned object, effectively
        // clearing out the contents of `m_delta` making it ready for the next
        // set of updates.
        return std::move(m_delta);
    }

private:
    entity_map *m_entity_map;

    map_of_component_map m_created_components;
    map_of_component_map m_updated_components;
    map_of_component_map m_destroyed_components;

    island_delta m_delta;
};

/**
 * @brief Implementation of `island_delta_builder` which allows a list of
 * support components to be specified.
 * 
 * @note
 * When users find the need to add extra logic to the physics simulation, they'll
 * need to have their custom components be shared between island coordinator and 
 * island worker so that their custom system update functions can work on these
 * components (functions assigned via `edyn::set_external_system_pre_step`, etc).
 * This class provides implementations of the functions that update the 
 * `island_delta` by component id, which is required for functionalities such
 * as marking all components as created/updated and marking components as dirty.
 * 
 * @tparam Component Pack of supported component types.
 */
template<typename... Component>
class island_delta_builder_impl: public island_delta_builder {
public:
    island_delta_builder_impl(entity_map &map, [[maybe_unused]] std::tuple<Component...>)
        : island_delta_builder(map)
    {}

    void created(entt::entity entity, entt::registry &registry, entt::id_type id) override {
        ((entt::type_index<Component>::value() == id ? 
            island_delta_builder::created<Component>(entity, registry) : (void)0), ...);
    }

    void created_all(entt::entity entity, entt::registry &registry) override {
        ((registry.has<Component>(entity) ? 
            island_delta_builder::created<Component>(entity, registry) : (void)0), ...);
    }

    void updated(entt::entity entity, entt::registry &registry, entt::id_type id) override {
        ((entt::type_index<Component>::value() == id ? 
            island_delta_builder::updated<Component>(entity, registry) : (void)0), ...);
    }

    void updated_all(entt::entity entity, entt::registry &registry) override {
        ((registry.has<Component>(entity) ? 
            island_delta_builder::updated<Component>(entity, registry) : (void)0), ...);
    }

    void destroyed(entt::entity entity, entt::id_type id) override {
        ((entt::type_index<Component>::value() == id ? 
            island_delta_builder::destroyed<Component>(entity) : (void)0), ...);
    }
};

/**
 * @brief Function type of a factory function that creates instances of a 
 * registry delta builder implementation.
 */
using make_island_delta_builder_func_t = std::unique_ptr<island_delta_builder>(*)(entity_map &);

/**
 * @brief Pointer to a factory function that makes new delta builders.
 * 
 * The default function returns a delta builder configured with all default 
 * shared components (`edyn::shared_components`) but it can be replaced by
 * a function that returns a builder which additionally handles external
 * components set by the user.
 */
extern make_island_delta_builder_func_t g_make_island_delta_builder;

/**
 * @brief Creates a new delta builder.
 * 
 * Returns a delta builder implementation that supports handling all shared 
 * component types plus any external component set by the user.
 * 
 * @return Safe pointer to an instance of a delta builder implementation.
 */
std::unique_ptr<island_delta_builder> make_island_delta_builder(entity_map &);

/**
 * @brief Registers external components to be shared between island coordinator
 * and island workers. 
 * @tparam Component External component types.
 */
template<typename... Component>
void register_external_components() {
    g_make_island_delta_builder = [] (entity_map &map) {
        auto external = std::tuple<Component...>{};
        auto all_components = std::tuple_cat(edyn::shared_components{}, external);
        return std::unique_ptr<edyn::island_delta_builder>(
            new edyn::island_delta_builder_impl(map, all_components));
    };
}

/**
 * @brief Removes registered external components and resets to defaults.
 */
void remove_external_components();

}

#endif // EDYN_PARALLEL_ISLAND_DELTA_BUILDER_HPP
