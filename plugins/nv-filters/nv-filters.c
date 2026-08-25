#include <obs-module.h>
/* Pulsar #167 / Prism ADR 023 Amendment 3 §A3.4 layer (i) --
 * plugins/pulsar-nv-secure-load/. */
#include <pulsar-nv-secure-load.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("nv-filters", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "NVIDIA filters";
}

#ifdef LIBNVAFX_ENABLED
extern struct obs_source_info nvidia_audiofx_filter;
extern bool load_nvidia_afx(void);
extern void unload_nvidia_afx(void);
#endif
#ifdef LIBNVVFX_ENABLED
extern struct obs_source_info nvidia_greenscreen_filter_info;
extern struct obs_source_info nvidia_blur_filter_info;
extern struct obs_source_info nvidia_background_blur_filter_info;
extern bool load_nvidia_vfx(void);
extern void unload_nvidia_vfx(void);
#endif

/* Pulsar #167 -- CONDITIONAL LOAD, and the first thing this module does.
 *
 * Upstream let the module load unconditionally and discover the absence of
 * an SDK inside load_nvidia_afx() / load_nvidia_vfx(), i.e. AFTER the two
 * environment-driven loaders had already run. That is the arrangement this
 * gate removes: with no valid SDK -- the ordinary case, since Pulsar ships
 * none -- obs_module_load() returns false here and neither loader is ever
 * reached. Returning false, rather than returning true with nothing
 * registered, is deliberate: libobs then leaves the module uninitialised
 * and registers nothing, and the criterion is the ABSENCE of loading, not
 * the presence of a warning (issue #167 criterion 4).
 *
 * The probe itself loads nothing: it reads directory validity, file
 * presence and version resources off absolute paths. The full result is
 * republished in the capability manifest by pulsar-multi-stream, so a
 * consumer sees WHY this module is or is not there without parsing a log.
 */
bool obs_module_load(void)
{
	struct pulsar_nv_probe_result probe;
	pulsar_nv_probe(&probe);

	if (!pulsar_nv_module_should_load(&probe)) {
		blog(LOG_INFO,
		     "[NVIDIA filters]: not loaded -- no validated NVIDIA SDK "
		     "(afx: dir=%d dll=%d models=%d version=%u/%u | "
		     "vfx: dir=%d dll=%d models=%d version=%u/%u)",
		     (int)probe.afx.dir_valid, (int)probe.afx.dlls_present, (int)probe.afx.models_present,
		     probe.afx.version, probe.afx.min_version, (int)probe.vfx.dir_valid, (int)probe.vfx.dlls_present,
		     (int)probe.vfx.models_present, probe.vfx.version, probe.vfx.min_version);
		return false;
	}

#ifdef LIBNVAFX_ENABLED
	/* load nvidia audio fx dll */
	if (load_nvidia_afx())
		obs_register_source(&nvidia_audiofx_filter);
#endif
#ifdef LIBNVVFX_ENABLED
	obs_enter_graphics();
	const bool direct3d = gs_get_device_type() == GS_DEVICE_DIRECT3D_11;
	obs_leave_graphics();
	if (direct3d && load_nvidia_vfx()) {
		obs_register_source(&nvidia_greenscreen_filter_info);
		obs_register_source(&nvidia_blur_filter_info);
		obs_register_source(&nvidia_background_blur_filter_info);
	}
#endif
	return true;
}

void obs_module_unload(void)
{
#ifdef LIBNVAFX_ENABLED
	unload_nvidia_afx();
#endif
#ifdef LIBNVVFX_ENABLED
	unload_nvidia_vfx();
#endif
}
