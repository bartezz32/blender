/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edrend
 */

#ifdef WITH_CYCLES

#  include "render_intern.hh" /* own include */

#  include <mutex>

#  include "MEM_guardedalloc.h"

#  include "DNA_image_types.h"
#  include "DNA_userdef_types.h"
#  include "DNA_windowmanager_enums.h"

#  include "BLI_fileops.h"
#  include "BLI_listbase.h"
#  include "BLI_path_utils.hh"
#  include "BLI_set.hh"
#  include "BLI_string.h"
#  include "BLI_task.hh"
#  include "BLI_vector.hh"

#  include "BKE_context.hh"
#  include "BKE_global.hh"
#  include "BKE_image.hh"
#  include "BKE_lib_query.hh"
#  include "BKE_main.hh"
#  include "BKE_node.hh"
#  include "BKE_report.hh"

#  include "BLT_translation.hh"

#  include "WM_api.hh"

#  include "CCL_api.h"

/* -------------------------------------------------------------------- */
/** \name Generate Texture Cache Operator
 * \{ */

struct GenerateTextureCacheJob {
  Main *bmain;
  ReportList *reports;
};

static void generate_texture_cache(Main *bmain,
                                   ReportList *reports,
                                   wmJobWorkerStatus *worker_status = nullptr)
{
  /* Gather images to generate for. */
  blender::Vector<const Image *> images;

  FOREACH_NODETREE_BEGIN (bmain, ntree, id) {
    if (ntree->type != NTREE_SHADER) {
      continue;
    }
    BKE_library_foreach_ID_link(
        bmain,
        &ntree->id,
        [&](LibraryIDLinkCallbackData *cb_data) {
          ID *image_id = *cb_data->id_pointer;
          if (image_id && GS(image_id->name) == ID_IM) {
            Image *image = blender::id_cast<Image *>(image_id);
            images.append(image);
          }
          return IDWALK_RET_NOP;
        },
        nullptr,
        IDWALK_READONLY);
  }
  FOREACH_NODETREE_END;

  /* Gather filepaths to generate for, expanding UDIMs and sequences. */
  blender::Set<std::pair<const Image *, std::string>> filepaths;

  for (const Image *image : images) {
    if (image->filepath[0] == '\0') {
      continue;
    }

    /* Get regular absolute path. */
    char filepath[FILE_MAX];
    STRNCPY(filepath, image->filepath);
    BLI_path_abs(filepath, ID_BLEND_PATH_FROM_GLOBAL(&image->id));
    BLI_path_normalize(filepath);

    /* Handle each UDIM tile. */
    if (image->source == IMA_SRC_TILED) {
      char file_name[FILE_MAX];
      BLI_path_split_file_part(filepath, file_name, FILE_MAX);
      eUDIM_TILE_FORMAT tile_format = UDIM_TILE_FORMAT_NONE;
      char *udim_pattern = BKE_image_get_tile_strformat(file_name, &tile_format);

      if (tile_format != UDIM_TILE_FORMAT_NONE) {
        LISTBASE_FOREACH (ImageTile *, tile, &image->tiles) {
          char tile_filepath[FILE_MAX];
          BKE_image_set_filepath_from_tile_number(
              tile_filepath, udim_pattern, tile_format, tile->tile_number);
          if (BLI_is_file(tile_filepath) &&
              !CCL_has_texture_cache(image, tile_filepath, U.texture_cachedir))
          {
            filepaths.add({image, tile_filepath});
          }
        }
        MEM_freeN(udim_pattern);
        continue;
      }
    }

    /* TODO: handle image sequences. */

    /* Handle regular image. */
    if (BLI_is_file(filepath) && !CCL_has_texture_cache(image, filepath, U.texture_cachedir)) {
      filepaths.add({image, filepath});
    }
  }

  /* Generate texture cache. */
  std::atomic<int> completed = 0;
  std::atomic<int> failed = 0;
  std::mutex reports_mutex;

  blender::threading::parallel_for_each(filepaths, [&](const auto &item) {
    if (worker_status) {
      if (worker_status->stop) {
        return;
      }
      worker_status->progress = (completed + failed) / float(filepaths.size());
      worker_status->do_update = true;
    }

    if (CCL_generate_texture_cache(item.first, item.second.c_str(), U.texture_cachedir)) {
      completed++;
    }
    else {
      std::scoped_lock lock(reports_mutex);
      BKE_reportf(
          reports, RPT_ERROR, "Failed to generate texture cache for: %s", item.second.c_str());
      failed++;
    }
  });

  /* Report stats. */
  if (failed == 0 && completed == 0) {
    BKE_reportf(reports, RPT_INFO, "All tx files up to date");
  }
  else if (failed) {
    BKE_reportf(
        reports, RPT_ERROR, "Generated %d tx files, failed %d", completed.load(), failed.load());
  }
  else {
    BKE_reportf(reports, RPT_INFO, "Generated %d tx files", completed.load());
  }
}

static wmOperatorStatus generate_texture_cache_exec(bContext *C, wmOperator *op)
{
  generate_texture_cache(CTX_data_main(C), op->reports);
  return OPERATOR_FINISHED;
}

static void generate_texture_cache_startjob(void *customdata, wmJobWorkerStatus *worker_status)
{
  GenerateTextureCacheJob *job = static_cast<GenerateTextureCacheJob *>(customdata);
  generate_texture_cache(job->bmain, job->reports, worker_status);
}

static wmOperatorStatus generate_texture_cache_invoke(bContext *C,
                                                      wmOperator *op,
                                                      const wmEvent * /*event*/)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  Main *bmain = CTX_data_main(C);

  wmJob *wm_job = WM_jobs_get(wm,
                              CTX_wm_window(C),
                              bmain,
                              RPT_("Generating texture cache..."),
                              WM_JOB_PRIORITY | WM_JOB_PROGRESS,
                              WM_JOB_TYPE_GENERATE_TEXTURE_CACHE);

  GenerateTextureCacheJob *job = MEM_new<GenerateTextureCacheJob>(__func__);
  job->bmain = bmain;
  job->reports = op->reports;
  WM_jobs_customdata_set(wm_job, job, [](void *customdata) {
    MEM_delete(static_cast<GenerateTextureCacheJob *>(customdata));
  });

  WM_jobs_timer(wm_job, 0.2, NC_WM | ND_JOB, 0);
  WM_jobs_callbacks(wm_job, generate_texture_cache_startjob, nullptr, nullptr, nullptr);

  G.is_break = false;
  WM_jobs_start(wm, wm_job);

  WM_event_add_modal_handler(C, op);

  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus generate_texture_cache_modal(bContext *C,
                                                     wmOperator * /*op*/,
                                                     const wmEvent *event)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  Main *bmain = CTX_data_main(C);

  if (0 == WM_jobs_test(wm, bmain, WM_JOB_TYPE_GENERATE_TEXTURE_CACHE)) {
    return OPERATOR_FINISHED | OPERATOR_PASS_THROUGH;
  }

  return (event->type == EVT_ESCKEY) ? OPERATOR_RUNNING_MODAL : OPERATOR_PASS_THROUGH;
}

static void generate_texture_cache_cancel(bContext *C, wmOperator * /*op*/)
{
  wmWindowManager *wm = CTX_wm_manager(C);
  Main *bmain = CTX_data_main(C);
  WM_jobs_kill_type(wm, bmain, WM_JOB_TYPE_GENERATE_TEXTURE_CACHE);
}

void RENDER_OT_generate_texture_cache(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Generate Texture Cache";
  ot->idname = "RENDER_OT_generate_texture_cache";
  ot->description = "Generate Cycles texture cache files for all images used in shader nodes";

  /* API callbacks. */
  ot->exec = generate_texture_cache_exec;
  ot->invoke = generate_texture_cache_invoke;
  ot->modal = generate_texture_cache_modal;
  ot->cancel = generate_texture_cache_cancel;

  /* flags */
  ot->flag = OPTYPE_REGISTER;
}

/** \} */

#endif
