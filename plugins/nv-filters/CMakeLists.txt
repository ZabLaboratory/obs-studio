cmake_minimum_required(VERSION 3.28...3.30)
if(OS_WINDOWS)
  add_library(nv-filters MODULE)
  add_library(OBS::nv-filters ALIAS nv-filters)

  option(ENABLE_NVAFX "Enable building with NVIDIA Audio Effects SDK (requires redistributable to be installed)" ON)
  option(ENABLE_NVVFX "Enable building with NVIDIA Video Effects SDK (requires redistributable to be installed)" ON)

  if(ENABLE_NVAFX)
    target_enable_feature(nv-filters "NVIDIA Audio FX support" LIBNVAFX_ENABLED)
    target_sources(nv-filters PRIVATE nvidia-audiofx-filter.c)
  else()
    target_disable_feature(nv-filters "NVIDIA Audio FX support")
  endif()

  if(ENABLE_NVVFX)
    target_enable_feature(nv-filters "NVIDIA Video FX support" LIBNVVFX_ENABLED)
    target_sources(nv-filters PRIVATE nvidia-videofx-filter.c)
  else()
    target_disable_feature(nv-filters "NVIDIA Video FX support")
  endif()

  configure_file(cmake/windows/obs-module.rc.in nv-filters.rc)
  target_sources(nv-filters PRIVATE nv-filters.rc)
  target_sources(nv-filters PRIVATE nv-filters.c)

  # Pulsar #167 -- pulsar-nv-secure-load.h lives in the Pulsar repo, one
  # level above this vendored tree, so it stays reviewable as source in the
  # PR instead of being buried in a patch blob. It needs version.lib for its
  # GetFileVersionInfo* probe (versions read off the file, never off a load)
  # and shell32/ole32 for SHGetKnownFolderPath, which resolves the VFX
  # fallback root from the system instead of from %ProgramFiles% (ADR 023
  # Am.3 §A3.4 rules an environment read out of path resolution). A move of
  # that directory has to be mirrored here in the same commit -- hence the
  # fail-fast rather than an obscure #include error.
  set(PULSAR_NV_SECURE_LOAD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../plugins/pulsar-nv-secure-load")
  get_filename_component(PULSAR_NV_SECURE_LOAD_DIR "${PULSAR_NV_SECURE_LOAD_DIR}" ABSOLUTE)
  if(NOT EXISTS "${PULSAR_NV_SECURE_LOAD_DIR}/pulsar-nv-secure-load.h")
    message(FATAL_ERROR "Pulsar #167: pulsar-nv-secure-load.h not found at ${PULSAR_NV_SECURE_LOAD_DIR}")
  endif()
  target_include_directories(nv-filters PRIVATE "${PULSAR_NV_SECURE_LOAD_DIR}")

  target_link_libraries(nv-filters PRIVATE OBS::libobs $<$<PLATFORM_ID:Windows>:OBS::w32-pthreads> version shell32
                                           ole32)

  set_target_properties_obs(nv-filters PROPERTIES FOLDER plugins PREFIX "")
endif()
