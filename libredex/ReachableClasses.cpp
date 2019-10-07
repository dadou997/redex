/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ReachableClasses.h"

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>

#include "ClassHierarchy.h"
#include "DexClass.h"
#include "Match.h"
#include "RedexResources.h"
#include "ReflectionAnalysis.h"
#include "StringUtil.h"
#include "TypeSystem.h"
#include "Walkers.h"

namespace {

using namespace reflection;

template <typename T, typename F>
struct DexItemIter {};

template <typename F>
struct DexItemIter<DexField*, F> {
  static void iterate(DexClass* cls, F& yield) {
    if (cls->is_external()) return;
    for (auto* field : cls->get_sfields()) {
      yield(field);
    }
    for (auto* field : cls->get_ifields()) {
      yield(field);
    }
  }
};

template <typename F>
struct DexItemIter<DexMethod*, F> {
  static void iterate(DexClass* cls, F& yield) {
    if (cls->is_external()) return;
    for (auto* method : cls->get_dmethods()) {
      yield(method);
    }
    for (auto* method : cls->get_vmethods()) {
      yield(method);
    }
  }
};

/*
 * Prevent a class from being deleted due to its being referenced via
 * reflection. :reflecting_method is the method containing the reflection site.
 */
void blacklist_field(DexMethod* reflecting_method,
                     DexType* type,
                     DexString* name,
                     bool declared) {
  auto* cls = type_class(type);
  if (cls == nullptr) {
    return;
  }
  auto yield = [&](DexField* t) {
    if (t->get_name() != name) {
      return;
    }
    if (!is_public(t) && !declared) {
      return;
    }
    TRACE(PGR, 4, "SRA BLACKLIST: %s", SHOW(t));
    t->rstate.set_root(keep_reason::REFLECTION, reflecting_method);
  };
  DexItemIter<DexField*, decltype(yield)>::iterate(cls, yield);
  if (!declared) {
    auto super_cls = cls->get_super_class();
    if (super_cls != nullptr) {
      blacklist_field(reflecting_method, super_cls, name, declared);
    }
  }
}

void blacklist_method(DexMethod* reflecting_method,
                      DexType* type,
                      DexString* name,
                      const boost::optional<std::vector<DexType*>>& params,
                      bool declared) {
  auto* cls = type_class(type);
  if (cls == nullptr) {
    return;
  }
  auto yield = [&](DexMethod* t) {
    if (t->get_name() != name) {
      return;
    }
    if (params != boost::none && !t->get_proto()->get_args()->equals(*params)) {
      return;
    }
    if (!is_public(t) && !declared) {
      return;
    }
    TRACE(PGR, 4, "SRA BLACKLIST: %s", SHOW(t));
    t->rstate.set_root(keep_reason::REFLECTION, reflecting_method);
  };
  DexItemIter<DexMethod*, decltype(yield)>::iterate(cls, yield);
  if (!declared) {
    auto super_cls = cls->get_super_class();
    if (super_cls != nullptr) {
      blacklist_method(reflecting_method, super_cls, name, params, declared);
    }
  }
}

void analyze_reflection(const Scope& scope) {
  enum ReflectionType {
    GET_FIELD,
    GET_DECLARED_FIELD,
    GET_METHOD,
    GET_DECLARED_METHOD,
    GET_CONSTRUCTOR,
    GET_DECLARED_CONSTRUCTOR,
    INT_UPDATER,
    LONG_UPDATER,
    REF_UPDATER,
  };

  const auto JAVA_LANG_CLASS = "Ljava/lang/Class;";
  const auto ATOMIC_INT_FIELD_UPDATER =
      "Ljava/util/concurrent/atomic/AtomicIntegerFieldUpdater;";
  const auto ATOMIC_LONG_FIELD_UPDATER =
      "Ljava/util/concurrent/atomic/AtomicLongFieldUpdater;";
  const auto ATOMIC_REF_FIELD_UPDATER =
      "Ljava/util/concurrent/atomic/AtomicReferenceFieldUpdater;";

  const std::unordered_map<std::string,
                           std::unordered_map<std::string, ReflectionType>>
      refls = {
          {JAVA_LANG_CLASS,
           {
               {"getField", GET_FIELD},
               {"getDeclaredField", GET_DECLARED_FIELD},
               {"getMethod", GET_METHOD},
               {"getDeclaredMethod", GET_DECLARED_METHOD},
               {"getConstructor", GET_CONSTRUCTOR},
               {"getConstructors", GET_CONSTRUCTOR},
               {"getDeclaredConstructor", GET_DECLARED_CONSTRUCTOR},
               {"getDeclaredConstructors", GET_DECLARED_CONSTRUCTOR},
           }},
          {ATOMIC_INT_FIELD_UPDATER,
           {
               {"newUpdater", INT_UPDATER},
           }},
          {ATOMIC_LONG_FIELD_UPDATER,
           {
               {"newUpdater", LONG_UPDATER},
           }},
          {ATOMIC_REF_FIELD_UPDATER,
           {
               {"newUpdater", REF_UPDATER},
           }},
      };

  auto dex_string_lookup = [](const ReflectionAnalysis& analysis,
                              ReflectionType refl_type,
                              IRInstruction* insn) {
    if (refl_type == GET_CONSTRUCTOR || refl_type == GET_DECLARED_CONSTRUCTOR) {
      return DexString::get_string("<init>");
    }
    int arg_str_idx = refl_type == ReflectionType::REF_UPDATER ? 2 : 1;
    auto arg_str = analysis.get_abstract_object(insn->src(arg_str_idx), insn);
    if (arg_str && arg_str->obj_kind == AbstractObjectKind::STRING) {
      return arg_str->dex_string;
    } else {
      return (DexString*)nullptr;
    }
  };

  walk::code(scope, [&](DexMethod* method, IRCode& code) {
    std::unique_ptr<ReflectionAnalysis> analysis = nullptr;
    for (auto& mie : InstructionIterable(code)) {
      IRInstruction* insn = mie.insn;
      if (!is_invoke(insn->opcode())) {
        continue;
      }

      // See if it matches something in refls
      auto& method_name = insn->get_method()->get_name()->str();
      auto& method_class_name =
          insn->get_method()->get_class()->get_name()->str();
      auto method_map = refls.find(method_class_name);
      if (method_map == refls.end()) {
        continue;
      }

      auto refl_entry = method_map->second.find(method_name);
      if (refl_entry == method_map->second.end()) {
        continue;
      }
      ReflectionType refl_type = refl_entry->second;

      // Instantiating the analysis object also runs the reflection analysis
      // on the method. So, we wait until we're sure we need it.
      // We use a unique_ptr so that we'll still only have one per method.
      if (!analysis) {
        analysis = std::make_unique<ReflectionAnalysis>(method);
      }

      auto arg_cls = analysis->get_abstract_object(insn->src(0), insn);
      if (!arg_cls || arg_cls->obj_kind != AbstractObjectKind::CLASS) {
        continue;
      }

      // Deal with methods that take a varying number of arguments.
      DexString* arg_str_value = dex_string_lookup(*analysis, refl_type, insn);
      if (arg_str_value == nullptr) {
        continue;
      }
      boost::optional<std::vector<DexType*>> param_types = boost::none;
      if (refl_type == GET_METHOD || refl_type == GET_CONSTRUCTOR ||
          refl_type == GET_DECLARED_METHOD ||
          refl_type == GET_DECLARED_CONSTRUCTOR) {
        param_types = analysis->get_method_params(insn);
      }
      TRACE(PGR, 4, "SRA ANALYZE: %s: type:%d %s.%s cls: %d %s %s str: %s",
            insn->get_method()->get_name()->str().c_str(), refl_type,
            method_class_name.c_str(), method_name.c_str(), arg_cls->obj_kind,
            SHOW(arg_cls->dex_type), SHOW(arg_cls->dex_string),
            SHOW(arg_str_value));

      switch (refl_type) {
      case GET_FIELD:
        blacklist_field(method, arg_cls->dex_type, arg_str_value, false);
        break;
      case GET_DECLARED_FIELD:
        blacklist_field(method, arg_cls->dex_type, arg_str_value, true);
        break;
      case GET_METHOD:
      case GET_CONSTRUCTOR:
        blacklist_method(method, arg_cls->dex_type, arg_str_value, param_types,
                         false);
        break;
      case GET_DECLARED_METHOD:
      case GET_DECLARED_CONSTRUCTOR:
        blacklist_method(method, arg_cls->dex_type, arg_str_value, param_types,
                         true);
        break;
      case INT_UPDATER:
      case LONG_UPDATER:
      case REF_UPDATER:
        blacklist_field(method, arg_cls->dex_type, arg_str_value, true);
        break;
      }
    }
  });
}

template <typename DexMember>
void mark_only_reachable_directly(DexMember* m) {
  m->rstate.ref_by_type();
}

/**
 * Indicates that a class is being used via reflection.
 *
 * Examples:
 *
 *   Bar.java:
 *     Object x = Class.forName("com.facebook.Foo").newInstance();
 *
 *   MyGreatLayout.xml:
 *     <com.facebook.MyTerrificView />
 */
void mark_reachable_by_classname(DexClass* dclass) {
  if (dclass == nullptr) return;
  dclass->rstate.ref_by_string();
  // When we mark a class as reachable, we also mark all fields and methods as
  // reachable.  Eventually we will be smarter about this, which will allow us
  // to remove unused methods and fields.
  for (DexMethod* dmethod : dclass->get_dmethods()) {
    dmethod->rstate.ref_by_string();
  }
  for (DexMethod* vmethod : dclass->get_vmethods()) {
    vmethod->rstate.ref_by_string();
  }
  for (DexField* sfield : dclass->get_sfields()) {
    sfield->rstate.ref_by_string();
  }
  for (DexField* ifield : dclass->get_ifields()) {
    ifield->rstate.ref_by_string();
  }
}

void mark_reachable_by_string(DexMethod* method) {
  if (method == nullptr) {
    return;
  }
  if (auto cls = type_class_internal(method->get_class())) {
    cls->rstate.ref_by_string();
  }
  method->rstate.ref_by_string();
}

void mark_reachable_by_classname(DexType* dtype) {
  mark_reachable_by_classname(type_class_internal(dtype));
}

// Possible methods for an android:onClick accept 1 argument that is a View.
// Source:
// https://android.googlesource.com/platform/frameworks/base/+/android-8.0.0_r15/core/java/android/view/View.java#5331
// Returns true if it matches that criteria, and it's in the set of known
// attribute values.
bool matches_onclick_method(const DexMethod* dmethod,
                            const std::set<std::string>& names_to_keep) {
  auto prototype = dmethod->get_proto();
  auto args_list = prototype->get_args();
  if (args_list->size() == 1) {
    auto first_type = args_list->get_type_list()[0];
    if (strcmp(first_type->c_str(), "Landroid/view/View;") == 0) {
      std::string method_name = dmethod->c_str();
      return names_to_keep.count(method_name) > 0;
    }
  }
  return false;
}

// Simulates aapt's generated keep statements for any View which has an
// android:onClick="foo" attribute.
// Example (from aapt):
// -keepclassmembers class * { *** foo(...); }
//
// This version however is much more specific, since keeping every method "foo"
// is overkill. We only need to keep methods "foo" defined on a subclass of
// android.content.Context that accept 1 argument (an android.view.View).
void mark_onclick_attributes_reachable(
    const Scope& scope, const std::set<std::string>& onclick_attribute_values) {
  if (onclick_attribute_values.size() == 0) {
    return;
  }
  auto type_context = DexType::get_type("Landroid/content/Context;");
  always_assert(type_context != nullptr);

  auto class_hierarchy = build_type_hierarchy(scope);
  TypeSet children;
  get_all_children(class_hierarchy, type_context, children);

  for (const auto& t : children) {
    auto dclass = type_class(t);
    if (dclass->is_external()) {
      continue;
    }
    // Methods are invoked via reflection. Only public methods are relevant.
    for (const auto& m : dclass->get_vmethods()) {
      if (matches_onclick_method(m, onclick_attribute_values)) {
        TRACE(PGR, 2, "Keeping vmethod %s due to onClick attribute in XML.",
              SHOW(m));
        m->rstate.set_referenced_by_resource_xml();
      }
    }
  }
}

DexClass* maybe_class_from_string(const std::string& classname) {
  auto dtype = DexType::get_type(classname.c_str());
  if (dtype == nullptr) {
    return nullptr;
  }
  auto dclass = type_class(dtype);
  if (dclass == nullptr) {
    return nullptr;
  }
  return dclass;
}

void mark_manifest_root(const std::string& classname) {
  auto dclass = maybe_class_from_string(classname);
  if (dclass == nullptr) {
    TRACE(PGR, 3, "Dangling reference from manifest: %s", classname.c_str());
    return;
  }
  TRACE(PGR, 3, "manifest: %s", classname.c_str());
  dclass->rstate.set_root(keep_reason::MANIFEST);
  // Prevent renaming.
  dclass->rstate.increment_keep_count();
  for (DexMethod* dmethod : dclass->get_ctors()) {
    dmethod->rstate.set_root(keep_reason::MANIFEST);
  }
}

/*
 * We mark an <activity>'s referenced class as reachable only if it is exported
 * or has intent filters. Exported Activities may be called from other apps, so
 * we must treat them as entry points. Activities with intent filters can be
 * called via implicit intents, and it is difficult to statically determine
 * which Activity an implicit intent will resolve to, so we treat all potential
 * recipient Activities as always reachable. For more details, see:
 *
 *   https://developer.android.com/guide/topics/manifest/activity-element
 *   https://developer.android.com/guide/components/intents-filters
 *
 * Note 1: Every Activity must be registered in the manifest before it can be
 * invoked by an intent (both explicit and implicit ones). Since our class
 * renamer isn't currently able to rewrite class names in the manifest, we mark
 * all Activities as non-obfuscatable.
 *
 * Note 2: RMU may delete some of the Activities that we haven't marked as entry
 * points. However, it currently doesn't know how to rewrite the manifest to
 * remove the corresponding <activity> tags. This seems benign: the Android
 * runtime appears to be OK with these dangling references.
 *
 * Addendum: The other component tags are also governed by the exported
 * attribute as well as by intent filters, but I (jezng) am not sure if those
 * are sufficient to statically determine their reachability, so I am taking the
 * conservative approach. This may be worth revisiting.
 */
void analyze_reachable_from_manifest(
    const std::string& apk_dir,
    const std::unordered_set<std::string>& prune_unexported_components_str) {
  std::unordered_map<std::string, ComponentTag> string_to_tag{
      {"activity", ComponentTag::Activity},
      {"activity-alias", ComponentTag::ActivityAlias}};
  std::unordered_set<ComponentTag, EnumClassHash> prune_unexported_components;
  for (const auto& s : prune_unexported_components_str) {
    prune_unexported_components.emplace(string_to_tag.at(s));
  }

  std::string manifest = apk_dir + std::string("/AndroidManifest.xml");
  const auto& manifest_class_info = get_manifest_class_info(manifest);

  for (const auto& classname : manifest_class_info.application_classes) {
    mark_manifest_root(classname);
  }

  for (const auto& classname : manifest_class_info.instrumentation_classes) {
    mark_manifest_root(classname);
  }

  for (const auto& tag_info : manifest_class_info.component_tags) {
    switch (tag_info.tag) {
    case ComponentTag::Activity:
    case ComponentTag::ActivityAlias: {
      if (tag_info.is_exported || tag_info.has_intent_filters ||
          !prune_unexported_components.count(tag_info.tag)) {
        mark_manifest_root(tag_info.classname);
      } else {
        TRACE(PGR, 3, "%s not exported", tag_info.classname.c_str());
        auto dclass = maybe_class_from_string(tag_info.classname);
        if (dclass) {
          dclass->rstate.increment_keep_count();
          dclass->rstate.unset_allowobfuscation();
        }
      }
      break;
    }
    case ComponentTag::Receiver:
    case ComponentTag::Service: {
      mark_manifest_root(tag_info.classname);
      break;
    }
    case ComponentTag::Provider: {
      mark_manifest_root(tag_info.classname);
      for (const auto& classname : tag_info.authority_classes) {
        mark_manifest_root(classname);
      }
      break;
    }
    }
  }
}

void mark_reachable_by_xml(const std::string& classname) {
  auto dclass = maybe_class_from_string(classname);
  if (dclass == nullptr) {
    return;
  }
  // Setting "referenced_by_resource_xml" essentially behaves like keep,
  // though breaking it out to its own flag will let us clear/recompute this.
  dclass->rstate.set_referenced_by_resource_xml();
  // Mark the constructors as used, which should be the expected use case from
  // layout inflation.
  for (DexMethod* dmethod : dclass->get_ctors()) {
    dmethod->rstate.set_referenced_by_resource_xml();
  }
}

// 1) Marks classes (Fragments, Views) found in XML layouts as reachable along
// with their constructors.
// 2) Marks candidate methods that could be called via android:onClick
// attributes.
void analyze_reachable_from_xml_layouts(const Scope& scope,
                                        const std::string& apk_dir) {
  std::unordered_set<std::string> layout_classes;
  std::unordered_set<std::string> attrs_to_read;
  // Method names used by reflection
  attrs_to_read.emplace(ONCLICK_ATTRIBUTE);
  std::unordered_multimap<std::string, std::string> attribute_values;
  collect_layout_classes_and_attributes(apk_dir, attrs_to_read, layout_classes,
                                        attribute_values);
  for (std::string classname : layout_classes) {
    TRACE(PGR, 3, "xml_layout: %s", classname.c_str());
    mark_reachable_by_xml(classname);
  }
  auto attr_values =
      multimap_values_to_set(attribute_values, ONCLICK_ATTRIBUTE);
  mark_onclick_attributes_reachable(scope, attr_values);
}

// Set is_serde to be true for all JSON serializer and deserializer classes
// that extend any one of supercls_names.
void initialize_reachable_for_json_serde(
    const Scope& scope, const std::vector<std::string>& supercls_names) {
  std::unordered_set<const DexType*> serde_superclses;
  for (auto& cls_name : supercls_names) {
    const DexType* supercls = DexType::get_type(cls_name);
    if (supercls) {
      serde_superclses.emplace(supercls);
    }
  }
  if (serde_superclses.size() == 0) {
    return;
  }
  ClassHierarchy ch = build_type_hierarchy(scope);
  TypeSet children;
  for (auto* serde_supercls : serde_superclses) {
    get_all_children(ch, serde_supercls, children);
    for (auto* child : children) {
      type_class(child)->rstate.set_is_serde();
    }
  }
}

template <typename DexMember>
bool anno_set_contains(DexMember m,
                       const std::unordered_set<DexType*>& keep_annotations) {
  auto const& anno_set = m->get_anno_set();
  if (anno_set == nullptr) return false;
  auto const& annos = anno_set->get_annotations();
  for (auto const& anno : annos) {
    if (keep_annotations.count(anno->type())) {
      return true;
    }
  }
  return false;
}

void keep_annotated_classes(
    const Scope& scope, const std::unordered_set<DexType*>& keep_annotations) {
  for (auto const& cls : scope) {
    if (anno_set_contains(cls, keep_annotations)) {
      mark_only_reachable_directly(cls);
    }
    for (auto const& m : cls->get_dmethods()) {
      if (anno_set_contains(m, keep_annotations)) {
        mark_only_reachable_directly(m);
      }
    }
    for (auto const& m : cls->get_vmethods()) {
      if (anno_set_contains(m, keep_annotations)) {
        mark_only_reachable_directly(m);
      }
    }
    for (auto const& m : cls->get_sfields()) {
      if (anno_set_contains(m, keep_annotations)) {
        mark_only_reachable_directly(m);
      }
    }
    for (auto const& m : cls->get_ifields()) {
      if (anno_set_contains(m, keep_annotations)) {
        mark_only_reachable_directly(m);
      }
    }
  }
}

/*
 * This method handles the keep_class_members from the configuration file.
 */
void keep_class_members(const Scope& scope,
                        const std::vector<std::string>& keep_class_mems) {
  for (auto const& cls : scope) {
    const std::string& name = cls->get_type()->get_name()->str();
    for (auto const& class_mem : keep_class_mems) {
      std::string class_mem_str = std::string(class_mem.c_str());
      std::size_t pos = class_mem_str.find(name);
      if (pos != std::string::npos) {
        std::string rem_str = class_mem_str.substr(pos + name.size());
        for (auto const& f : cls->get_sfields()) {
          if (rem_str.find(f->get_name()->str()) != std::string::npos) {
            mark_only_reachable_directly(f);
            mark_only_reachable_directly(cls);
          }
        }
        break;
      }
    }
  }
}

void keep_methods(const Scope& scope, const std::vector<std::string>& ms) {
  std::set<std::string> methods_to_keep(ms.begin(), ms.end());
  for (auto const& cls : scope) {
    for (auto& m : cls->get_dmethods()) {
      if (methods_to_keep.count(m->get_name()->c_str())) {
        m->rstate.ref_by_string();
      }
    }
    for (auto& m : cls->get_vmethods()) {
      if (methods_to_keep.count(m->get_name()->c_str())) {
        m->rstate.ref_by_string();
      }
    }
  }
}

/*
 * Returns true iff this class or any of its super classes are in the set of
 * classes banned due to use of complex reflection.
 */
bool in_reflected_pkg(DexClass* dclass,
                      std::unordered_set<DexClass*>& reflected_pkg_classes) {
  if (dclass == nullptr) {
    // Not in our dex files
    return false;
  }

  if (reflected_pkg_classes.count(dclass)) {
    return true;
  }
  return in_reflected_pkg(type_class_internal(dclass->get_super_class()),
                          reflected_pkg_classes);
}

/**
 * Mark serializable class's non-serializable super class's no arg constructor
 * as root.
 */
void analyze_serializable(const Scope& scope) {
  DexType* serializable = DexType::get_type("Ljava/io/Serializable;");
  if (!serializable) {
    return;
  }
  TypeSet children;
  get_all_implementors(scope, serializable, children);

  for (auto* child : children) {
    DexClass* child_cls = type_class(child);
    DexType* child_super_type = child_cls->get_super_class();
    DexClass* child_supercls = type_class(child_super_type);
    if (!child_supercls || child_supercls->is_external()) {
      continue;
    }
    // We should keep the no argument constructors of the superclasses of
    // any Serializable class, if they are themselves not Serializable.
    if (!children.count(child_super_type)) {
      for (auto meth : child_supercls->get_dmethods()) {
        if (is_init(meth) && meth->get_proto()->get_args()->size() == 0) {
          meth->rstate.set_root(keep_reason::SERIALIZABLE);
        }
      }
    }
  }
}

/*
 * Initializes list of classes that are reachable via reflection, and calls
 * or from code.
 *
 * These include:
 *  - Classes used in the manifest (e.g. activities, services, etc)
 *  - View or Fragment classes used in layouts
 *  - Classes that are in certain packages (specified in the reflected_packages
 *    section of the config) and classes that extend from them
 *  - Classes marked with special annotations (keep_annotations in config)
 *  - Classes reachable from native libraries
 */
void init_permanently_reachable_classes(
    const Scope& scope,
    const JsonWrapper& config,
    const std::unordered_set<DexType*>& no_optimizations_anno) {

  std::string apk_dir;
  std::vector<std::string> reflected_package_names;
  std::vector<std::string> annotations;
  std::vector<std::string> class_members;
  std::vector<std::string> methods;
  std::unordered_set<std::string> prune_unexported_components;
  bool compute_xml_reachability;
  bool analyze_native_lib_reachability;

  config.get("apk_dir", "", apk_dir);
  config.get("keep_packages", {}, reflected_package_names);
  config.get("keep_annotations", {}, annotations);
  config.get("keep_class_members", {}, class_members);
  config.get("keep_methods", {}, methods);
  config.get("compute_xml_reachability", true, compute_xml_reachability);
  config.get("prune_unexported_components", {}, prune_unexported_components);
  config.get("analyze_native_lib_reachability", true,
             analyze_native_lib_reachability);

  std::unordered_set<DexType*> annotation_types(no_optimizations_anno.begin(),
                                                no_optimizations_anno.end());

  for (auto const& annostr : annotations) {
    DexType* anno = DexType::get_type(annostr.c_str());
    if (anno) {
      annotation_types.insert(anno);
    } else {
      fprintf(stderr, "WARNING: keep annotation %s not found\n",
              annostr.c_str());
    }
  }

  keep_annotated_classes(scope, annotation_types);
  keep_class_members(scope, class_members);
  keep_methods(scope, methods);

  if (apk_dir.size()) {
    if (compute_xml_reachability) {
      // Classes present in manifest
      analyze_reachable_from_manifest(apk_dir, prune_unexported_components);
      // Classes present in XML layouts
      analyze_reachable_from_xml_layouts(scope, apk_dir);
    }

    if (analyze_native_lib_reachability) {
      // Classnames present in native libraries (lib/*/*.so)
      for (std::string classname : get_native_classes(apk_dir)) {
        auto type = DexType::get_type(classname.c_str());
        if (type == nullptr) continue;
        TRACE(PGR, 3, "native_lib: %s", classname.c_str());
        mark_reachable_by_classname(type);
      }
    }
  }

  analyze_reflection(scope);

  std::unordered_set<DexClass*> reflected_package_classes;
  for (auto clazz : scope) {
    const char* cname = clazz->get_type()->get_name()->c_str();
    for (auto pkg : reflected_package_names) {
      if (starts_with(cname, pkg.c_str())) {
        reflected_package_classes.insert(clazz);
        continue;
      }
    }
  }
  for (auto clazz : scope) {
    if (in_reflected_pkg(clazz, reflected_package_classes)) {
      reflected_package_classes.insert(clazz);
      /* Note:
       * Some of these are by string, others by type
       * but we have no way in the config to distinguish
       * them currently.  So, we mark with the most
       * conservative sense here.
       */
      TRACE(PGR, 3, "reflected_package: %s", SHOW(clazz));
      mark_reachable_by_classname(clazz);
    }
  }
  analyze_serializable(scope);
}

/**
 * Walks all the code of the app, finding classes that are reachable from
 * code.
 *
 * Note that as code is changed or removed by Redex, this information will
 * become stale, so this method should be called periodically, for example
 * after each pass.
 */
void recompute_classes_reachable_from_code(const Scope& scope) {
  // Matches methods marked as native
  walk::methods(scope, [&](DexMethod* meth) {
    if (meth->get_access() & DexAccessFlags::ACC_NATIVE) {
      TRACE(PGR, 3, "native_method: %s", SHOW(meth->get_class()));
      mark_reachable_by_string(meth);
    }
  });
}

} // namespace

void recompute_reachable_from_xml_layouts(const Scope& scope,
                                          const std::string& apk_dir) {
  walk::parallel::classes(scope, [](DexClass* cls) {
    cls->rstate.unset_referenced_by_resource_xml();
    for (auto* method : cls->get_dmethods()) {
      method->rstate.unset_referenced_by_resource_xml();
    }
    for (auto* method : cls->get_vmethods()) {
      method->rstate.unset_referenced_by_resource_xml();
    }
    for (auto* field : cls->get_ifields()) {
      field->rstate.unset_referenced_by_resource_xml();
    }
    for (auto* field : cls->get_sfields()) {
      field->rstate.unset_referenced_by_resource_xml();
    }
  });
  analyze_reachable_from_xml_layouts(scope, apk_dir);
}

void init_reachable_classes(
    const Scope& scope,
    const JsonWrapper& config,
    const std::unordered_set<DexType*>& no_optimizations_anno) {

  // Find classes that are reachable in such a way that none of the redex
  // passes will cause them to be no longer reachable.  For example, if a
  // class is referenced from the manifest.
  init_permanently_reachable_classes(scope, config, no_optimizations_anno);

  // Classes that are reachable in ways that could change as Redex runs. For
  // example, a class might be instantiated from a method, but if that method
  // is later deleted then it might no longer be reachable.
  recompute_classes_reachable_from_code(scope);

  std::vector<std::string> json_serde_supercls;
  config.get("json_serde_supercls", {}, json_serde_supercls);
  initialize_reachable_for_json_serde(scope, json_serde_supercls);
}

std::string ReferencedState::str() const {
  std::ostringstream s;
  s << inner_struct.m_by_type;
  s << inner_struct.m_by_string;
  s << inner_struct.m_by_resources;
  s << inner_struct.m_is_serde;
  s << inner_struct.m_keep;
  s << allowshrinking();
  s << allowobfuscation();
  s << inner_struct.m_assumenosideeffects;
  s << inner_struct.m_blanket_keepnames;
  s << inner_struct.m_whyareyoukeeping;
  s << ' ';
  s << m_keep_count;
  return s.str();
}
