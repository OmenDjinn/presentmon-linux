// SPDX-License-Identifier: MIT
#include "pml/drm_outputs.hpp"

#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <glob.h>
#include <mutex>
#include <type_traits>
#include <unistd.h>

#ifdef PML_HAVE_LIBDRM_HEADERS
#include <xf86drm.h>
#include <xf86drmMode.h>
#endif

namespace pml {

#ifdef PML_HAVE_LIBDRM_HEADERS

namespace {

// libdrm entry points, resolved at run time.
struct DrmApi
{
	decltype(&drmModeGetResources) GetResources = nullptr;
	decltype(&drmModeFreeResources) FreeResources = nullptr;
	decltype(&drmModeGetConnectorCurrent) GetConnectorCurrent = nullptr;
	decltype(&drmModeFreeConnector) FreeConnector = nullptr;
	decltype(&drmModeGetEncoder) GetEncoder = nullptr;
	decltype(&drmModeFreeEncoder) FreeEncoder = nullptr;
	decltype(&drmModeObjectGetProperties) ObjectGetProperties = nullptr;
	decltype(&drmModeFreeObjectProperties) FreeObjectProperties = nullptr;
	decltype(&drmModeGetProperty) GetProperty = nullptr;
	decltype(&drmModeFreeProperty) FreeProperty = nullptr;
	decltype(&drmModeGetPropertyBlob) GetPropertyBlob = nullptr;
	decltype(&drmModeFreePropertyBlob) FreePropertyBlob = nullptr;
	decltype(&drmModeGetCrtc) GetCrtc = nullptr;
	decltype(&drmModeFreeCrtc) FreeCrtc = nullptr;
	const char *(*GetConnectorTypeName)(uint32_t) = nullptr;   // libdrm >= 2.4.112, optional
	decltype(&drmGetVersion) GetVersion = nullptr;             // optional (the driver's name)
	decltype(&drmFreeVersion) FreeVersion = nullptr;
	bool ok = false;
};

const DrmApi &drmApi()
{
	static DrmApi api;
	static std::once_flag once;
	std::call_once(once, [] {
		void *lib = dlopen("libdrm.so.2", RTLD_NOW | RTLD_LOCAL);
		if (!lib)
			return;   // kept loaded for the process's life when found
		auto sym = [&](auto &fn, const char *name) {
			fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
			return fn != nullptr;
		};
		bool ok = true;
		ok = sym(api.GetResources, "drmModeGetResources") && ok;
		ok = sym(api.FreeResources, "drmModeFreeResources") && ok;
		ok = sym(api.GetConnectorCurrent, "drmModeGetConnectorCurrent") && ok;
		ok = sym(api.FreeConnector, "drmModeFreeConnector") && ok;
		ok = sym(api.GetEncoder, "drmModeGetEncoder") && ok;
		ok = sym(api.FreeEncoder, "drmModeFreeEncoder") && ok;
		ok = sym(api.ObjectGetProperties, "drmModeObjectGetProperties") && ok;
		ok = sym(api.FreeObjectProperties, "drmModeFreeObjectProperties") && ok;
		ok = sym(api.GetProperty, "drmModeGetProperty") && ok;
		ok = sym(api.FreeProperty, "drmModeFreeProperty") && ok;
		ok = sym(api.GetPropertyBlob, "drmModeGetPropertyBlob") && ok;
		ok = sym(api.FreePropertyBlob, "drmModeFreePropertyBlob") && ok;
		ok = sym(api.GetCrtc, "drmModeGetCrtc") && ok;
		ok = sym(api.FreeCrtc, "drmModeFreeCrtc") && ok;
		sym(api.GetConnectorTypeName, "drmModeGetConnectorTypeName");
		if (!sym(api.GetVersion, "drmGetVersion") || !sym(api.FreeVersion, "drmFreeVersion"))
			api.GetVersion = nullptr;
		api.ok = ok;
	});
	return api;
}

const char *connectorTypeName(uint32_t type)
{
	switch (type)
	{
	case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
	case DRM_MODE_CONNECTOR_HDMIA: return "HDMI-A";
	case DRM_MODE_CONNECTOR_HDMIB: return "HDMI-B";
	case DRM_MODE_CONNECTOR_eDP: return "eDP";
	case DRM_MODE_CONNECTOR_DVID: return "DVI-D";
	case DRM_MODE_CONNECTOR_DVII: return "DVI-I";
	case DRM_MODE_CONNECTOR_VGA: return "VGA";
	case DRM_MODE_CONNECTOR_USB: return "USB";
	default: return "Unknown";
	}
}

uint64_t propertyValue(const DrmApi &api, int fd, uint32_t object, uint32_t type, const char *name, bool *found)
{
	*found = false;
	uint64_t value = 0;
	drmModeObjectProperties *props = api.ObjectGetProperties(fd, object, type);
	if (!props)
		return 0;
	for (uint32_t i = 0; i < props->count_props && !*found; i++)
	{
		drmModePropertyRes *p = api.GetProperty(fd, props->props[i]);
		if (p && !std::strcmp(p->name, name))
		{
			value = props->prop_values[i];
			*found = true;
		}
		if (p)
			api.FreeProperty(p);
	}
	api.FreeObjectProperties(props);
	return value;
}

void readCard(const DrmApi &api, const char *path, std::vector<DrmOutput> *out)
{
	const int fd = ::open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return;
	drmModeRes *res = api.GetResources(fd);
	if (!res)
	{
		::close(fd);
		return;
	}
	std::string driver;
	if (api.GetVersion)
		if (drmVersion *v = api.GetVersion(fd))
		{
			if (v->name && v->name_len > 0)
				driver.assign(v->name, size_t(v->name_len));
			api.FreeVersion(v);
		}
	for (int i = 0; i < res->count_connectors; i++)
	{
		drmModeConnector *c = api.GetConnectorCurrent(fd, res->connectors[i]);
		if (!c)
			continue;
		if (c->connection != DRM_MODE_CONNECTED)
		{
			api.FreeConnector(c);
			continue;
		}
		DrmOutput o;
		o.driver = driver;
		const char *type = api.GetConnectorTypeName ? api.GetConnectorTypeName(c->connector_type) : nullptr;
		o.name = std::string(type ? type : connectorTypeName(c->connector_type)) + "-" + std::to_string(c->connector_type_id);
		bool found = false;
		o.capable = propertyValue(api, fd, c->connector_id, DRM_MODE_OBJECT_CONNECTOR, "vrr_capable", &found) != 0;
		const uint64_t blobId = propertyValue(api, fd, c->connector_id, DRM_MODE_OBJECT_CONNECTOR, "EDID", &found);
		if (found && blobId)
		{
			if (drmModePropertyBlobRes *blob = api.GetPropertyBlob(fd, uint32_t(blobId)))
			{
				edidVerticalRange(static_cast<const uint8_t *>(blob->data), blob->length, &o.edidMinHz, &o.edidMaxHz);
				api.FreePropertyBlob(blob);
			}
		}
		uint32_t crtcId = 0;
		if (c->encoder_id)
			if (drmModeEncoder *e = api.GetEncoder(fd, c->encoder_id))
			{
				crtcId = e->crtc_id;
				api.FreeEncoder(e);
			}
		if (crtcId)
		{
			const uint64_t v = propertyValue(api, fd, crtcId, DRM_MODE_OBJECT_CRTC, "VRR_ENABLED", &found);
			o.enabled = found ? (v != 0 ? 1 : 0) : -1;
			if (drmModeCrtc *crtc = api.GetCrtc(fd, crtcId))
			{
				const drmModeModeInfo &m = crtc->mode;
				if (crtc->mode_valid && m.htotal && m.vtotal)
					o.refreshHz = double(m.clock) * 1000.0 / (double(m.htotal) * double(m.vtotal));
				if (crtc->mode_valid)
				{
					o.width = m.hdisplay;
					o.height = m.vdisplay;
				}
				api.FreeCrtc(crtc);
			}
		}
		out->push_back(o);
		api.FreeConnector(c);
	}
	api.FreeResources(res);
	::close(fd);
}

} // namespace

std::vector<DrmOutput> readDrmOutputs(std::string *error)
{
	std::vector<DrmOutput> out;
	const DrmApi &api = drmApi();
	if (!api.ok)
	{
		if (error)
			*error = "libdrm.so.2 not available";
		return out;
	}
	glob_t g = {};
	if (::glob("/dev/dri/card*", 0, nullptr, &g) == 0)
	{
		for (size_t i = 0; i < g.gl_pathc; i++)
			readCard(api, g.gl_pathv[i], &out);
	}
	::globfree(&g);
	if (out.empty() && error)
		*error = "no readable DRM output";
	return out;
}

#else

std::vector<DrmOutput> readDrmOutputs(std::string *error)
{
	if (error)
		*error = "built without libdrm headers";
	return {};
}

#endif

bool edidVerticalRange(const uint8_t *edid, size_t size, double *minHz, double *maxHz)
{
	if (!edid || size < 128)
		return false;
	for (size_t off : {54u, 72u, 90u, 108u})
	{
		const uint8_t *d = edid + off;
		if (d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 0xFD)
		{
			const uint8_t flags = d[4];
			*minHz = double(d[5]) + ((flags & 0x01) ? 255.0 : 0.0);
			*maxHz = double(d[6]) + ((flags & 0x02) ? 255.0 : 0.0);
			return *minHz > 0 && *maxHz >= *minHz;
		}
	}
	return false;
}

namespace {

// The pickVrrOutput rule among the outputs `allowed` lets through.
template <typename Allowed>
const DrmOutput *bestVrrOutput(const std::vector<DrmOutput> &outputs, uint32_t width, uint32_t height, Allowed &&allowed)
{
	const DrmOutput *best = nullptr;
	int bestRank = -1;
	for (const DrmOutput &o : outputs)
	{
		if (o.enabled != 1 || !allowed(o))
			continue;
		int rank = 0;
		if (width && height && o.width == width && o.height == height)
			rank = 2;
		else if (width && height && o.width && o.height && (o.width >= o.height) == (width >= height))
			rank = 1;
		if (rank > bestRank || (rank == bestRank && best && o.refreshHz > best->refreshHz))
		{
			best = &o;
			bestRank = rank;
		}
	}
	return best;
}

} // namespace

const DrmOutput *pickVrrOutput(const std::vector<DrmOutput> &outputs, uint32_t width, uint32_t height)
{
	return bestVrrOutput(outputs, width, height, [](const DrmOutput &) { return true; });
}

const DrmOutput *pickOutput(const std::vector<DrmOutput> &outputs, uint32_t width, uint32_t height,
                            uint64_t refreshDurationNs)
{
	// No allocation: the frame cap asks on every present while Auto is selected.
	const double hz = refreshDurationNs ? 1.0e9 / double(refreshDurationNs) : 0.0;
	const auto lit = [](const DrmOutput &o) { return o.refreshHz > 0; };   // no CRTC: nothing is shown there
	const auto matches = [&](const DrmOutput &o) {
		return lit(o) && hz > 0 && std::fabs(o.refreshHz - hz) <= hz * kRefreshMatch;
	};
	size_t matching = 0, shown = 0;
	for (const DrmOutput &o : outputs)
	{
		matching += matches(o) ? 1 : 0;
		shown += lit(o) ? 1 : 0;
	}
	// The outputs the swapchain can be on: those whose mode has its refresh cycle, else every lit one.
	const bool byRefresh = matching > 0;
	const auto candidate = [&](const DrmOutput &o) { return byRefresh ? matches(o) : lit(o); };
	const size_t count = byRefresh ? matching : shown;
	if (count == 0)
		return nullptr;
	const DrmOutput *first = nullptr;
	for (const DrmOutput &o : outputs)
		if (candidate(o))
		{
			first = &o;
			break;
		}
	if (count == 1)
		return first;
	if (const DrmOutput *on = bestVrrOutput(outputs, width, height, candidate))
		return on;
	// Several candidates, none with VRR on. With the refresh cycle known they all run that mode with VRR off, and
	// which one it is changes nothing but the name; without it the game's screen is unknown.
	return byRefresh ? first : nullptr;
}

bool rescansAtVrrFloor(const DrmOutput &output)
{
	return output.driver == "nvidia-drm";
}

} // namespace pml
