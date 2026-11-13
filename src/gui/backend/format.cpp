// SPDX-License-Identifier: MIT
#include "format.hpp"

#include <pml/metrics/adapter.hpp>

#include <vulkan/vulkan_core.h>

#include <QRegularExpression>

#include <cmath>

namespace pml::gui {

QString presentModeName(uint32_t vkPresentMode)
{
	const QString name = QString::fromLatin1(pml::metrics::presentModeName(vkPresentMode));
	if (name == QLatin1String("Unknown"))
		return QStringLiteral("Unknown (%1)").arg(vkPresentMode);
	return name;
}

QString colorSpaceName(uint32_t vkColorSpace)
{
	switch (vkColorSpace)
	{
	case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR: return QStringLiteral("sRGB");
	case VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT: return QStringLiteral("Display P3");
	case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT: return QStringLiteral("scRGB (linear)");
	case VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT: return QStringLiteral("Display P3 (linear)");
	case VK_COLOR_SPACE_DCI_P3_NONLINEAR_EXT: return QStringLiteral("DCI-P3");
	case VK_COLOR_SPACE_BT709_LINEAR_EXT: return QStringLiteral("BT.709 (linear)");
	case VK_COLOR_SPACE_BT709_NONLINEAR_EXT: return QStringLiteral("BT.709");
	case VK_COLOR_SPACE_BT2020_LINEAR_EXT: return QStringLiteral("BT.2020 (linear)");
	case VK_COLOR_SPACE_HDR10_ST2084_EXT: return QStringLiteral("HDR10 (PQ)");
	case VK_COLOR_SPACE_HDR10_HLG_EXT: return QStringLiteral("HDR10 (HLG)");
	case VK_COLOR_SPACE_ADOBERGB_LINEAR_EXT: return QStringLiteral("Adobe RGB (linear)");
	case VK_COLOR_SPACE_ADOBERGB_NONLINEAR_EXT: return QStringLiteral("Adobe RGB");
	case VK_COLOR_SPACE_PASS_THROUGH_EXT: return QStringLiteral("pass-through");
	case VK_COLOR_SPACE_EXTENDED_SRGB_NONLINEAR_EXT: return QStringLiteral("scRGB");
	default: return QStringLiteral("Colour space %1").arg(vkColorSpace);
	}
}

bool isHdrColorSpace(uint32_t vkColorSpace)
{
	switch (vkColorSpace)
	{
	case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
	case VK_COLOR_SPACE_EXTENDED_SRGB_NONLINEAR_EXT:
	case VK_COLOR_SPACE_BT2020_LINEAR_EXT:
	case VK_COLOR_SPACE_HDR10_ST2084_EXT:
	case VK_COLOR_SPACE_HDR10_HLG_EXT:
		return true;
	default:
		return false;
	}
}

QString runtimeName(const pml::SwapchainInfo &info)
{
	const std::string_view engine(info.engineName, strnlen(info.engineName, sizeof(info.engineName)));
	return QString::fromLatin1(pml::metrics::runtimeName(engine));
}

QString swapchainSummary(const pml::SwapchainInfo &info)
{
	QStringList parts;
	parts << QStringLiteral("%1×%2").arg(info.width).arg(info.height);
	parts << presentModeName(info.presentMode);
	if (info.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
		parts << colorSpaceName(info.colorSpace);
	parts << runtimeName(info);
	return parts.join(QStringLiteral(" · "));
}

namespace {

QString sanitize(const QString &name)
{
	QString out;
	out.reserve(name.size());
	for (QChar c : name)
	{
		const bool ok = (c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) ||
		                (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || c == QLatin1Char('.') || c == QLatin1Char('_') ||
		                c == QLatin1Char('+') || c == QLatin1Char('-');
		out += ok ? c : QLatin1Char('_');
	}
	return out.isEmpty() ? QStringLiteral("process") : out;
}

} // namespace

QString captureFileName(const QString &processName, const QDateTime &when)
{
	return QStringLiteral("pmcap-%1-%2.csv").arg(sanitize(processName), when.toString(QStringLiteral("yyMMdd-HHmmss")));
}

bool parseCaptureFileName(const QString &fileName, QString *processName, QDateTime *when)
{
	static const QRegularExpression re(QStringLiteral("^pmcap-(.+)-(\\d{6}-\\d{6})\\.csv$"));
	const QRegularExpressionMatch m = re.match(fileName);
	if (!m.hasMatch())
		return false;
	QDateTime t = QDateTime::fromString(m.captured(2), QStringLiteral("yyMMdd-HHmmss"));
	if (!t.isValid())
		return false;
	t = t.addYears(100);   // "yy" parses as 19yy
	if (processName)
		*processName = m.captured(1);
	if (when)
		*when = t;
	return true;
}

QString vrrDetailText(const pml::metrics::VrrReadout &v)
{
	using pml::metrics::VrrState;
	if (v.state == VrrState::NoData)
		return {};
	if (v.state == VrrState::Off)
	{
		if (std::isnan(v.onGridPct))
			return QStringLiteral("%1 frames").arg(qulonglong(v.frames));
		return QStringLiteral("%1 % of the screen updates on the %2 Hz grid · %3 frames")
		    .arg(v.onGridPct, 0, 'f', 1)
		    .arg(v.refreshHz, 0, 'f', 0)
		    .arg(qulonglong(v.frames));
	}
	QString detail = QStringLiteral("5th–95th percentile %1–%2 Hz · uneven %3 % (%4 % on screen only) · %5 %6")
	                     .arg(v.lowHz, 0, 'f', 0)
	                     .arg(v.highHz, 0, 'f', 0)
	                     .arg(v.unevenPct, 0, 'f', 1)
	                     .arg(v.unevenDisplayPct, 0, 'f', 1)
	                     .arg(qulonglong(v.stutters))
	                     .arg(v.stutters == 1 ? QStringLiteral("stutter") : QStringLiteral("stutters"));
	if (!std::isnan(v.rescans))
		detail += QStringLiteral(" · %1 %2 without a new frame (below the VRR floor)")
		              .arg(qulonglong(v.rescans))
		              .arg(v.rescans == 1 ? QStringLiteral("refresh") : QStringLiteral("refreshes"));
	else if (!std::isnan(v.lfcCrossings))
		detail += QStringLiteral(" · %1 crossings of the VRR floor").arg(qulonglong(v.lfcCrossings));
	detail += QStringLiteral(" · %1 frames in %2 s").arg(qulonglong(v.judgedFrames)).arg(v.judgedSeconds, 0, 'f', 1);
	return detail;
}

} // namespace pml::gui
