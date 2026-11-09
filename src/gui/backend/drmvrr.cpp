// SPDX-License-Identifier: MIT
#include "drmvrr.hpp"

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cstring>
#include <fcntl.h>
#include <glob.h>
#include <unistd.h>

namespace pml::gui {

namespace {

uint64_t propertyValue(int fd, uint32_t object, uint32_t type, const char *name, bool *found)
{
	*found = false;
	uint64_t value = 0;
	drmModeObjectProperties *props = drmModeObjectGetProperties(fd, object, type);
	if (!props)
		return 0;
	for (uint32_t i = 0; i < props->count_props && !*found; i++)
	{
		drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
		if (p && !std::strcmp(p->name, name))
		{
			value = props->prop_values[i];
			*found = true;
		}
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return value;
}

void readCard(const char *path, std::vector<OutputVrr> *out)
{
	const int fd = ::open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return;
	drmModeRes *res = drmModeGetResources(fd);
	if (!res)
	{
		::close(fd);
		return;
	}
	for (int i = 0; i < res->count_connectors; i++)
	{
		drmModeConnector *c = drmModeGetConnectorCurrent(fd, res->connectors[i]);
		if (!c)
			continue;
		if (c->connection != DRM_MODE_CONNECTED)
		{
			drmModeFreeConnector(c);
			continue;
		}
		OutputVrr o;
		const char *type = drmModeGetConnectorTypeName(c->connector_type);
		o.name = std::string(type ? type : "Unknown") + "-" + std::to_string(c->connector_type_id);
		bool found = false;
		o.capable = propertyValue(fd, c->connector_id, DRM_MODE_OBJECT_CONNECTOR, "vrr_capable", &found) != 0;
		const uint64_t blobId = propertyValue(fd, c->connector_id, DRM_MODE_OBJECT_CONNECTOR, "EDID", &found);
		if (found && blobId)
		{
			if (drmModePropertyBlobRes *blob = drmModeGetPropertyBlob(fd, uint32_t(blobId)))
			{
				edidVerticalRange(static_cast<const uint8_t *>(blob->data), blob->length, &o.edidMinHz, &o.edidMaxHz);
				drmModeFreePropertyBlob(blob);
			}
		}
		uint32_t crtcId = 0;
		if (c->encoder_id)
			if (drmModeEncoder *e = drmModeGetEncoder(fd, c->encoder_id))
			{
				crtcId = e->crtc_id;
				drmModeFreeEncoder(e);
			}
		if (crtcId)
		{
			const uint64_t v = propertyValue(fd, crtcId, DRM_MODE_OBJECT_CRTC, "VRR_ENABLED", &found);
			o.enabled = found ? (v != 0 ? 1 : 0) : -1;
			if (drmModeCrtc *crtc = drmModeGetCrtc(fd, crtcId))
			{
				const drmModeModeInfo &m = crtc->mode;
				if (crtc->mode_valid && m.htotal && m.vtotal)
					o.refreshHz = double(m.clock) * 1000.0 / (double(m.htotal) * double(m.vtotal));
				drmModeFreeCrtc(crtc);
			}
		}
		out->push_back(o);
		drmModeFreeConnector(c);
	}
	drmModeFreeResources(res);
	::close(fd);
}

} // namespace

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

std::vector<OutputVrr> readDrmOutputs(std::string *error)
{
	std::vector<OutputVrr> out;
	glob_t g = {};
	if (::glob("/dev/dri/card*", 0, nullptr, &g) == 0)
	{
		for (size_t i = 0; i < g.gl_pathc; i++)
			readCard(g.gl_pathv[i], &out);
	}
	::globfree(&g);
	if (out.empty() && error)
		*error = "no readable DRM output";
	return out;
}

pml::metrics::VrrContext vrrContextFromOutputs(const std::vector<OutputVrr> &outputs, std::string *which)
{
	pml::metrics::VrrContext ctx;
	if (which)
		which->clear();
	const OutputVrr *on = nullptr;
	size_t readable = 0, onCount = 0;
	for (const OutputVrr &o : outputs)
	{
		if (o.enabled < 0)
			continue;
		readable++;
		if (o.enabled == 1)
		{
			onCount++;
			on = &o;
		}
	}
	if (onCount == 1)
	{
		ctx.driver = pml::metrics::DriverVrr::On;
		ctx.refreshHz = on->refreshHz;
		ctx.vrrMinHz = on->edidMinHz;
		if (which)
			*which = on->name;
	}
	else if (readable > 0 && onCount == 0)
	{
		ctx.driver = pml::metrics::DriverVrr::Off;
	}
	return ctx;
}

} // namespace pml::gui
