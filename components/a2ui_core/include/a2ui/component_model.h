// Copyright 2026 The a2ui-lgvl Authors
// SPDX-License-Identifier: Apache-2.0
//
// ComponentModel and SurfaceComponentsModel (blueprint section E, "State
// Models"), ported from the Swift core's State/ComponentModel.swift and
// State/SurfaceComponentsModel.swift, with the graph checks of
// Validation/GraphTopologyValidator.swift and the incremental bound of
// Validation/ComponentGraphBound.swift.
//
// Components live in a flat map keyed by id. A component's children are
// read from its catalog's reference map (ComponentApi::child_refs).
//
// Graph validation is a phase that runs before mutation against a candidate
// graph (the held components overlaid with the batch); add/remove never
// validate. See validate_components_update.

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "a2ui/catalog.h"
#include "a2ui/error.h"
#include "a2ui/events.h"
#include "a2ui/json.h"
#include "a2ui/validation_config.h"

namespace a2ui {

/** One reference from a component to another. */
struct ChildRef {
  std::string id;
  /** JSON Pointer of the reference inside the component ("/children/0"). */
  std::string path;
};

class ComponentModel {
 public:
  /**
   * `properties` holds everything but `id`, `component`, `@catalogId` and
   * `@metadata` (v1.0 keeps `@accessibility` and `@checks` there).
   * `catalog` is the catalog that declares the component; never null.
   */
  ComponentModel(std::string id, std::string type, CatalogPtr catalog, Json properties,
                 Json metadata = Json());
  ComponentModel(const ComponentModel&) = delete;
  ComponentModel& operator=(const ComponentModel&) = delete;

  const std::string& id() const { return id_; }
  const std::string& type() const { return type_; }
  const Catalog& catalog() const { return *catalog_; }
  const CatalogPtr& catalog_ptr() const { return catalog_; }
  /** The component's declaration in its catalog, or null for an unknown type. */
  const ComponentApi* api() const { return api_; }

  const Json& properties() const { return properties_; }
  /** Replaces the properties; fires on_updated when they changed. */
  void set_properties(Json properties);

  // MetadataHolder (ComponentCommon.@metadata).
  const Json& metadata() const { return metadata_; }
  const Json& extensions() const { return metadata_["extensions"]; }
  Status set_metadata(const Json& metadata);
  Status set_extension(const std::string& key, Json value);
  void remove_extension(const std::string& key);
  const EventSource<Json>& on_metadata_changed() const { return on_metadata_changed_; }

  /**
   * Applies an updateComponents entry for the same type and catalog: the
   * entry replaces the definition in full. Runs in one Batch; on_updated
   * fires only when the properties changed.
   */
  void apply_update(Json properties, Json metadata);

  /** Fires after the properties change. */
  const EventSource<const ComponentModel*>& on_updated() const { return on_updated_; }

  /** The component as a v1.0 wire object ({id, component, ...properties, @metadata?}). */
  Json component_tree() const;

  /** Appends this component's child references in property order. */
  void child_references(std::vector<ChildRef>* out) const;

  void dispose();

  /** The order the holding SurfaceComponentsModel added the component in. */
  uint64_t insertion_order() const { return seq_; }

 private:
  friend class SurfaceComponentsModel;

  std::string id_;
  std::string type_;
  CatalogPtr catalog_;
  const ComponentApi* api_;
  Json properties_;
  Json metadata_;
  EventSource<const ComponentModel*> on_updated_;
  EventSource<Json> on_metadata_changed_;
  // Set while held by a SurfaceComponentsModel: its change counter and the
  // order the component was added in.
  uint64_t* owner_revision_ = nullptr;
  uint64_t seq_ = 0;
};

/** Child references of a component given as a type, catalog and properties. */
void child_references_of(const ComponentApi* api, const Json& properties, std::vector<ChildRef>* out);

class ComponentGraphBound;

/** Inputs of one graph validation pass. */
struct GraphCheckOptions {
  std::string root_id = "root";
  std::string surface_id;
};

class SurfaceComponentsModel {
 public:
  using Map = std::map<std::string, std::unique_ptr<ComponentModel>, std::less<>>;
  using Batch = std::vector<std::unique_ptr<ComponentModel>>;

  SurfaceComponentsModel();
  ~SurfaceComponentsModel();
  SurfaceComponentsModel(const SurfaceComponentsModel&) = delete;
  SurfaceComponentsModel& operator=(const SurfaceComponentsModel&) = delete;

  ComponentModel* get(std::string_view id) const;
  const Map& components() const { return components_; }
  size_t size() const { return components_.size(); }
  bool has(std::string_view id) const { return components_.find(id) != components_.end(); }

  /** Mutation guard only: ErrorKind::State if the id is already present. */
  Status add_component(std::unique_ptr<ComponentModel> component);
  /** Removes and disposes; fires on_deleted. No-op for unknown ids. */
  void remove_component(const std::string& id);
  /** Replaces (or adds) a component; fires on_deleted for the old one, then on_created. */
  void replace_component(std::unique_ptr<ComponentModel> component);

  /** Child component ids referenced by a component, from the catalog reference map. */
  std::vector<std::string> get_child_ids(std::string_view id) const;

  /**
   * Validates an inbound batch against the held components overlaid with
   * it, before anything is applied: duplicate ids within the batch
   * (Integrity), composition constraints (allowedParents/allowedChildren,
   * with the surface as the root's parent; Validation, codes
   * UNALLOWED_PARENT / UNALLOWED_CHILD) and, when `config` is set, root
   * presence, dangling references, self-references, cycles and depth
   * (Recursion) and reachability (Integrity). Schema checks of each
   * component are the processor's job. Without a config only duplicates
   * and composition constraints are checked.
   *
   * Under a config that allows dangling references and orphans or a
   * missing root, the model keeps a bound of the checked graph, so the
   * next batch is checked in time proportional to what it changes. Call
   * batch_applied() after applying the batch so the bound carries over.
   */
  Status validate_components_update(const Batch& batch, const std::optional<ValidationConfig>& config,
                                    const GraphCheckOptions& options);
  /** Reports that the batch last validated has been applied. */
  void batch_applied();

  /** Root, dangling references, cycles and depth, then reachability, on the held graph. */
  Status validate_topology(const ValidationConfig& config, const std::string& root_id = "root") const;
  /** Self-references, cycles and depth from the root; returns the reachable ids. */
  Result<std::set<std::string>> detect_cycles(const ValidationConfig& config,
                                              const std::string& root_id = "root") const;
  /** Non-throwing validate_topology: the first error, or empty. */
  std::vector<Error> validate_references(const ValidationConfig& config,
                                         const std::string& root_id = "root") const;

  const EventSource<ComponentModel*>& on_created() const { return on_created_; }
  const EventSource<std::string>& on_deleted() const { return on_deleted_; }

  /** Counts changes to the held components (adds, removals, property updates). */
  uint64_t revision() const { return revision_; }

  void dispose();

 private:
  void note_change() { ++revision_; }
  void adopt(ComponentModel* c);

  Map components_;
  uint64_t revision_ = 0;
  uint64_t next_seq_ = 0;
  EventSource<ComponentModel*> on_created_;
  EventSource<std::string> on_deleted_;
  std::unique_ptr<ComponentGraphBound> bound_;
  uint64_t bound_revision_ = 0;
  bool bound_pending_ = false;
};

}  // namespace a2ui
