// SPDX-License-Identifier: MIT
#include "frameseries.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
}

FrameSeries::FrameSeries(QObject *parent) : QObject(parent) {}

int FrameSeries::count() const { return int(points_.size()); }
double FrameSeries::minX() const { return points_.isEmpty() ? kNaN : points_.front().x(); }
double FrameSeries::maxX() const { return points_.isEmpty() ? kNaN : points_.back().x(); }

double FrameSeries::maxY() const
{
	if (points_.isEmpty())
		return kNaN;
	if (maxYDirty_)
	{
		double m = -std::numeric_limits<double>::infinity();
		for (const QPointF &p : points_)
			if (p.y() > m)
				m = p.y();
		maxY_ = m;
		maxYDirty_ = false;
	}
	return maxY_;
}

QString FrameSeries::name() const { return name_; }

void FrameSeries::setName(const QString &name)
{
	if (name == name_)
		return;
	name_ = name;
	Q_EMIT nameChanged();
}

const QList<QPointF> &FrameSeries::points() const { return points_; }

void FrameSeries::append(double x, double y)
{
	if (!points_.isEmpty() && x < points_.back().x())
		return;
	points_.append(QPointF(x, y));
	if (!maxYDirty_ && y > maxY_)
		maxY_ = y;
}

void FrameSeries::trimBefore(double x)
{
	const auto it = std::lower_bound(points_.cbegin(), points_.cend(), x,
	                                 [](const QPointF &p, double v) { return p.x() < v; });
	const qsizetype n = it - points_.cbegin();
	if (n > 0)
	{
		points_.remove(0, n);
		maxYDirty_ = true;
	}
}

void FrameSeries::setPoints(QList<QPointF> points)
{
	if (!std::is_sorted(points.cbegin(), points.cend(), [](const QPointF &a, const QPointF &b) { return a.x() < b.x(); }))
		std::stable_sort(points.begin(), points.end(), [](const QPointF &a, const QPointF &b) { return a.x() < b.x(); });
	points_ = std::move(points);
	maxYDirty_ = true;
}

void FrameSeries::clear()
{
	points_.clear();
	maxYDirty_ = true;
}

void FrameSeries::notify() { Q_EMIT changed(); }

double FrameSeries::valueAt(double x) const
{
	const auto it = std::upper_bound(points_.cbegin(), points_.cend(), x,
	                                 [](double v, const QPointF &p) { return v < p.x(); });
	if (it == points_.cbegin())
		return kNaN;
	return std::prev(it)->y();
}
