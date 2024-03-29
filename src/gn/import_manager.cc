// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gn/import_manager.h"

#include <memory>

#include "gn/err.h"
#include "gn/parse_tree.h"
#include "gn/scheduler.h"
#include "gn/scope_per_file_provider.h"
#include "gn/trace.h"
#include "util/ticks.h"

namespace {

// Returns a newly-allocated scope on success, null on failure.
std::unique_ptr<Scope> UncachedImport(const Settings* settings,
                                      const SourceFile& file,
                                      const ParseNode* node_for_err,
                                      Scope* override_scope,
                                      Err* err) {
  ScopedTrace load_trace(TraceItem::TRACE_IMPORT_LOAD, file.value());
  load_trace.SetToolchain(settings->toolchain_label());

  const ParseNode* node = g_scheduler->input_file_manager()->SyncLoadFile(
      node_for_err->GetRange(), settings->build_settings(), file, err);
  if (!node)
    return nullptr;

  Scope* actual_scope = override_scope;
  std::unique_ptr<Scope> scope =
      std::make_unique<Scope>(settings->base_config());
  if (!actual_scope)
    actual_scope = scope.get();
  actual_scope->set_source_dir(file.GetDir());

  // Don't allow ScopePerFileProvider to provide target-related variables.
  // These will be relative to the imported file, which is probably not what
  // people mean when they use these.
  ScopePerFileProvider per_file_provider(actual_scope, false);

  actual_scope->SetProcessingImport();
  node->Execute(actual_scope, err);
  if (err->has_error()) {
    // If there was an error, append the caller location so the error message
    // displays a why the file was imported (esp. useful for failed asserts).
    err->AppendSubErr(Err(node_for_err, "whence it was imported."));
    return nullptr;
  }
  actual_scope->ClearProcessingImport();

  return scope;
}

}  // namespace

struct ImportManager::ImportInfo {
  ImportInfo() = default;
  ~ImportInfo() = default;

  // This lock protects the unique_ptr. Once the scope is computed,
  // it is const and can be accessed read-only outside of the lock.
  std::mutex load_lock;

  std::unique_ptr<const Scope> scope;

  // The result of loading the import. If the load failed, the scope will be
  // null but this will be set to error. In this case the thread should not
  // attempt to load the file, even if the scope is null.
  Err load_result;
};

ImportManager::ImportManager() = default;

ImportManager::~ImportManager() = default;

bool ImportManager::DoImport(const SourceFile& file,
                             const ParseNode* node_for_err,
                             Scope* scope,
                             Err* err) {
  // Key for the current import on the current thread in imports_in_progress_.
  std::stringstream ss;
  ss << std::this_thread::get_id() << file.value();
  std::string key = ss.str();

  // See if we have a cached import, but be careful to actually do the scope
  // copying outside of the lock.
  ImportInfo* import_info = nullptr;
  std::unique_ptr<ImportInfo> temp_import_info;
  Scope* override_scope = nullptr;
  if (scope->IsProcessingBuildConfig()) {
    // Don't cache build config imports
    temp_import_info = std::make_unique<ImportInfo>();
    import_info = temp_import_info.get();
    override_scope = scope;

    if (imports_in_progress_.find(key) != imports_in_progress_.end()) {
      *err = Err(Location(), file.value() + " is part of an import loop.");
      return false;
    }
    imports_in_progress_.insert(key);
  } else
  {
    std::lock_guard<std::mutex> lock(imports_lock_);
    std::unique_ptr<ImportInfo>& info_ptr = imports_[file];
    if (!info_ptr)
      info_ptr = std::make_unique<ImportInfo>();

    // Promote the ImportInfo to outside of the imports lock.
    import_info = info_ptr.get();

    if (imports_in_progress_.find(key) != imports_in_progress_.end()) {
      *err = Err(Location(), file.value() + " is part of an import loop.");
      return false;
    }
    imports_in_progress_.insert(key);
  }

  // Now use the per-import-file lock to block this thread if another thread
  // is already processing the import.
  const Scope* import_scope = nullptr;
  {
    Ticks import_block_begin = TicksNow();
    std::lock_guard<std::mutex> lock(import_info->load_lock);

    if (!import_info->scope) {
      // Only load if the import hasn't already failed.
      if (!import_info->load_result.has_error()) {
        import_info->scope = UncachedImport(
            scope->settings(), file, node_for_err, override_scope,
            &import_info->load_result);
      }
      if (import_info->load_result.has_error()) {
        *err = import_info->load_result;
        return false;
      }
    } else {
      // Add trace if this thread was blocked for a long period of time and did
      // not load the import itself.
      Ticks import_block_end = TicksNow();
      constexpr auto kImportBlockTraceThresholdMS = 20;
      if (TracingEnabled() &&
          TicksDelta(import_block_end, import_block_begin).InMilliseconds() >
              kImportBlockTraceThresholdMS) {
        auto* import_block_trace =
            new TraceItem(TraceItem::TRACE_IMPORT_BLOCK, file.value(),
                          std::this_thread::get_id());
        import_block_trace->set_begin(import_block_begin);
        import_block_trace->set_end(import_block_end);
        import_block_trace->set_toolchain(
            scope->settings()->toolchain_label().GetUserVisibleName(false));
        AddTrace(import_block_trace);
      }
    }

    // Promote the now-read-only scope to outside the load lock.
    import_scope = import_info->scope.get();
  }

  if (override_scope)
    return true;

  Scope::MergeOptions options;
  options.skip_private_vars = true;
  options.mark_dest_used = true;  // Don't require all imported values be used.

  {
    std::lock_guard<std::mutex> lock(imports_lock_);
    imports_in_progress_.erase(key);
  }

  return import_scope->NonRecursiveMergeTo(scope, options, node_for_err,
                                           "import", err);
}

std::vector<SourceFile> ImportManager::GetImportedFiles() const {
  std::vector<SourceFile> imported_files;
  imported_files.resize(imports_.size());
  std::transform(imports_.begin(), imports_.end(), imported_files.begin(),
                 [](const ImportMap::value_type& val) { return val.first; });
  return imported_files;
}
