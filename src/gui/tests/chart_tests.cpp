// SPDX-License-Identifier: MIT
// FrameTimeChart tests. The chart lives in a real QQuickWindow (offscreen platform), but that
// platform only offers the Qt Quick software backend, which cannot draw custom QSGGeometryNodes
// (grabWindow() comes back without the lines). So the tests take the scene graph tree the chart
// builds and rasterize its triangles on the CPU (pixel centres, flat colours, source-over), then
// check pixels exactly like a grab would. This covers the geometry and its node reuse; the GPU
// rasterization itself is Qt's.
#include "test_support.hpp"

#include "framechart.hpp"
#include "frameseries.hpp"

#include <QElapsedTimer>
#include <QImage>
#include <QQuickWindow>
#include <QSGFlatColorMaterial>
#include <QSGGeometryNode>

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

namespace {

constexpr int kW = 800, kH = 300;
const QColor kRed(255, 0, 0), kGreen(0, 255, 0);

bool isColor(QRgb p, const QColor &c, int tol = 40)
{
	return std::abs(qRed(p) - c.red()) <= tol && std::abs(qGreen(p) - c.green()) <= tol && std::abs(qBlue(p) - c.blue()) <= tol;
}

// CPU rasterizer for the chart's tree: DrawTriangles + Point2D + QSGFlatColorMaterial.
void rasterize(QSGNode *node, QImage &img)
{
	if (node->type() == QSGNode::GeometryNodeType)
	{
		auto *gn = static_cast<QSGGeometryNode *>(node);
		const QSGGeometry *g = gn->geometry();
		const QColor c = static_cast<QSGFlatColorMaterial *>(gn->material())->color();
		ASSERT_EQ(g->drawingMode(), QSGGeometry::DrawTriangles);
		ASSERT_EQ(g->vertexCount() % 3, 0);
		const QSGGeometry::Point2D *v = g->vertexDataAsPoint2D();
		for (int t = 0; t + 2 < g->vertexCount(); t += 3)
		{
			const auto &a = v[t], &b = v[t + 1], &d = v[t + 2];
			const float area = (b.x - a.x) * (d.y - a.y) - (b.y - a.y) * (d.x - a.x);
			if (area == 0)
				continue;
			const int x0 = std::max(0, int(std::floor(std::min({a.x, b.x, d.x})))), x1 = std::min(img.width() - 1, int(std::ceil(std::max({a.x, b.x, d.x}))));
			const int y0 = std::max(0, int(std::floor(std::min({a.y, b.y, d.y})))), y1 = std::min(img.height() - 1, int(std::ceil(std::max({a.y, b.y, d.y}))));
			for (int y = y0; y <= y1; ++y)
				for (int x = x0; x <= x1; ++x)
				{
					const float px = x + 0.5f, py = y + 0.5f;
					const float w0 = (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
					const float w1 = (d.x - b.x) * (py - b.y) - (d.y - b.y) * (px - b.x);
					const float w2 = (a.x - d.x) * (py - d.y) - (a.y - d.y) * (px - d.x);
					const bool in = area > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0) : (w0 <= 0 && w1 <= 0 && w2 <= 0);
					if (!in)
						continue;
					const QRgb dst = img.pixel(x, y);
					const double al = c.alphaF();
					img.setPixel(x, y, qRgb(int(c.red() * al + qRed(dst) * (1 - al) + 0.5), int(c.green() * al + qGreen(dst) * (1 - al) + 0.5),
					                        int(c.blue() * al + qBlue(dst) * (1 - al) + 0.5)));
				}
		}
	}
	for (QSGNode *ch = node->firstChild(); ch; ch = ch->nextSibling())
		rasterize(ch, img);
}

class ChartTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		window_ = std::make_unique<QQuickWindow>();
		window_->setColor(Qt::black);
		window_->resize(kW, kH);
		chart_ = new FrameTimeChart(window_->contentItem());
		chart_->setSize(QSizeF(kW, kH));
		chart_->setColors({kRed, kGreen});
		chart_->setGridColor(QColor(0, 0, 255, 255));
		chart_->setFollow(false);
		chart_->setLineWidth(2);
	}
	void TearDown() override
	{
		for (FrameSeries *s : owned_)
			delete s;
		delete root_;
		window_.reset();
	}

	FrameSeries *makeSeries()
	{
		owned_ << new FrameSeries;
		return owned_.back();
	}
	void assign(const QList<FrameSeries *> &list)
	{
		QList<QObject *> objs;
		for (FrameSeries *s : list)
			objs << s;
		chart_->setSeries(objs);
	}
	// Fills n points with x from x0 to x1 and y(x); one notify().
	template<class F>
	FrameSeries *fill(int n, double x0, double x1, F y)
	{
		FrameSeries *s = makeSeries();
		QList<QPointF> pts;
		pts.reserve(n);
		for (int i = 0; i < n; ++i)
		{
			const double x = x0 + (x1 - x0) * i / std::max(1, n - 1);
			pts.append(QPointF(x, y(x, i)));
		}
		s->setPoints(std::move(pts));
		return s;
	}
	QImage render()
	{
		QSGNode *before = root_;
		root_ = chart_->buildSceneGraphForTest(root_);
		EXPECT_TRUE(root_);
		if (before)
		{
			EXPECT_EQ(root_, before);   // reused, not rebuilt
		}
		QImage img(kW, kH, QImage::Format_ARGB32);
		img.fill(Qt::black);
		if (root_)
			rasterize(root_, img);
		return img;
	}
	// Number of pixels of the color in rows [y0,y1] x columns [x0,x1].
	static int count(const QImage &img, const QColor &c, int x0, int x1, int y0, int y1)
	{
		int n = 0;
		for (int y = std::max(0, y0); y <= std::min(img.height() - 1, y1); ++y)
			for (int x = std::max(0, x0); x <= std::min(img.width() - 1, x1); ++x)
				n += isColor(img.pixel(x, y), c);
		return n;
	}

	std::unique_ptr<QQuickWindow> window_;
	FrameTimeChart *chart_ = nullptr;
	QList<FrameSeries *> owned_;
	QSGNode *root_ = nullptr;
};

} // namespace

TEST_F(ChartTest, SceneGraphNodesAreReusedAcrossFrames)
{
	auto *s = fill(3000, 0, 10, [](double, int i) { return 5.0 + (i % 7); });
	assign({s});
	render();
	QList<QSGNode *> nodes;
	for (QSGNode *n = root_->firstChild(); n; n = n->nextSibling())
		nodes << n;
	ASSERT_EQ(nodes.size(), 3);   // grid, reference lines, one series
	for (int i = 0; i < 10; ++i)
	{
		s->append(10.0 + i * 0.003, 6);
		s->notify();
		render();
	}
	QList<QSGNode *> again;
	for (QSGNode *n = root_->firstChild(); n; n = n->nextSibling())
		again << n;
	EXPECT_EQ(again, nodes);
	// The real window path (software backend here, which ignores custom geometry) must not choke on it.
	window_->show();
	pmlguitest::waitUntil([&] { return window_->isExposed(); });
	EXPECT_FALSE(window_->grabWindow().isNull());
	EXPECT_GT(chart_->lastVertexCount(), 0);
}

TEST_F(ChartTest, ConstantSeriesDrawnAtItsValue)
{
	auto *s = fill(3000, 0, 10, [](double, int) { return 10.0; });
	assign({s});
	chart_->setYMax(20);
	const QImage img = render();
	ASSERT_EQ(img.width(), kW);
	const int y10 = int(std::lround(chart_->yToPixel(10)));
	EXPECT_EQ(y10, kH / 2);
	// A solid line across the whole width at y10, and nothing red further than the line width away.
	EXPECT_GT(count(img, kRed, 5, kW - 5, y10 - 2, y10 + 2), (kW - 10) * 2 - 100);
	EXPECT_EQ(count(img, kRed, 0, kW - 1, 0, y10 - 4), 0);
	EXPECT_EQ(count(img, kRed, 0, kW - 1, y10 + 4, kH - 1), 0);
	// The grid is drawn (blue) at the ticks, including 20 at the top.
	EXPECT_GT(count(img, QColor(0, 0, 255), 0, kW - 1, 0, kH - 1), kW * 2);
}

TEST_F(ChartTest, SpikeSurvivesDecimation)
{
	const int n = 200000;
	auto *s = fill(n, 0, 10, [&](double, int i) { return i == 123457 ? 100.0 : 7.0; });
	assign({s});
	chart_->setYMax(120);
	EXPECT_EQ(s->count(), n);
	const QImage img = render();
	const int col = int(std::lround(chart_->xToPixel(s->points()[123457].x())));
	const int top = int(chart_->yToPixel(95));
	// The vertical spike line reaches (almost) 100 ms in its column, nowhere else up there.
	EXPECT_GT(count(img, kRed, col - 2, col + 2, top, top + 3), 0);
	EXPECT_EQ(count(img, kRed, 0, col - 6, 0, int(chart_->yToPixel(20))), 0);
	EXPECT_EQ(count(img, kRed, col + 6, kW - 1, 0, int(chart_->yToPixel(20))), 0);
	EXPECT_LE(chart_->lastVertexCount(), 16 * kW);
}

TEST_F(ChartTest, GapsLeaveEmptyColumns)
{
	QList<QPointF> pts;
	for (int i = 0; i < 10000; ++i)
	{
		const double x = i * 0.001;
		if (x > 3.0 && x < 5.0)
			continue;
		pts.append(QPointF(x, 10));
	}
	auto *s = makeSeries();
	s->setPoints(pts);
	assign({s});
	chart_->setYMax(20);
	chart_->setXMin(0);
	chart_->setXMax(10);
	const QImage img = render();
	const int a = int(chart_->xToPixel(3.0)), b = int(chart_->xToPixel(5.0));
	EXPECT_EQ(count(img, kRed, a + 6, b - 6, 0, kH - 1), 0);
	EXPECT_GT(count(img, kRed, a - 40, a - 6, 0, kH - 1), 20);
	EXPECT_GT(count(img, kRed, b + 6, b + 40, 0, kH - 1), 20);
}

TEST_F(ChartTest, ReferenceLinesAreDashed)
{
	chart_->setYMax(40);
	chart_->setReferenceColor(kGreen);
	chart_->setReferenceLines({16.6667, 100});   // the second is above the top: not drawn
	const QImage img = render();
	const int y = int(chart_->yToPixel(16.6667));
	int hit = 0, rows = 0;
	for (int dy = -1; dy <= 1; ++dy)
	{
		const int c = count(img, kGreen, 0, kW - 1, y + dy, y + dy);
		if (c > 0)
			++rows;
		hit += c;
	}
	EXPECT_EQ(rows, 1);
	EXPECT_GT(hit, kW * 4 / 10);
	EXPECT_LT(hit, kW * 8 / 10);   // dashed, not solid
}

TEST_F(ChartTest, FollowShowsOnlyTheLastWindow)
{
	auto *s = fill(3000, 0, 30, [](double x, int) { return x < 25 ? 5.0 : 15.0; });
	assign({s});
	chart_->setFollow(true);
	chart_->setWindowSeconds(5);
	chart_->setYMax(20);
	EXPECT_NEAR(chart_->visibleXMax(), 30, 1e-9);
	EXPECT_NEAR(chart_->visibleXMin(), 25, 1e-9);
	const QImage img = render();
	EXPECT_EQ(count(img, kRed, 0, kW - 1, int(chart_->yToPixel(5)) - 3, int(chart_->yToPixel(5)) + 3), 0);
	EXPECT_GT(count(img, kRed, 5, kW - 5, int(chart_->yToPixel(15)) - 2, int(chart_->yToPixel(15)) + 2), kW);

	// A new batch moves the window with the data.
	for (int i = 0; i < 100; ++i)
		s->append(30.0 + i * 0.01, 15);
	s->notify();
	EXPECT_NEAR(chart_->visibleXMax(), 30.99, 1e-9);
	EXPECT_NEAR(chart_->visibleXMin(), 25.99, 1e-9);
}

TEST_F(ChartTest, AutoRangeIgnoresRareSpikesAndHasRoundTicks)
{
	auto *s = fill(3000, 0, 10, [](double, int i) { return (i == 500 || i == 1500 || i == 2500) ? 200.0 : 7.0; });
	assign({s});
	chart_->setYMax(0);
	EXPECT_GE(chart_->visibleYMax(), 10);
	EXPECT_LE(chart_->visibleYMax(), 25);
	const auto ticks = chart_->yTicks();
	EXPECT_GE(ticks.size(), 4);
	EXPECT_LE(ticks.size(), 7);
	EXPECT_EQ(ticks.first(), 0);
	for (double t : ticks)
	{
		EXPECT_GE(t, 0);
		EXPECT_LE(t, chart_->visibleYMax());
		const double r = t / 0.5;   // every tick is a multiple of 0.5 ms here
		EXPECT_NEAR(r, std::round(r), 1e-9) << t;
	}
	for (double t : chart_->xTicks())
	{
		EXPECT_GE(t, chart_->visibleXMin());
		EXPECT_LE(t, chart_->visibleXMax());
	}
	EXPECT_GE(chart_->xTicks().size(), 4);
	EXPECT_LE(chart_->xTicks().size(), 7);

	// Spikes are clipped at the top edge: the line there is drawn, inside the item.
	const QImage img = render();
	EXPECT_GT(count(img, kRed, 0, kW - 1, 0, 3), 0);

	// The top is stable: more steady frames do not change it (and no signal is emitted).
	int emitted = 0;
	QObject ctx;   // declared after `emitted`: the connection dies before the counter does
	QObject::connect(chart_, &FrameTimeChart::rangeChanged, &ctx, [&] { ++emitted; });
	const double top = chart_->visibleYMax();
	s->notify();
	EXPECT_EQ(emitted, 0);
	EXPECT_EQ(chart_->visibleYMax(), top);
}

TEST_F(ChartTest, AutoRangeGrowsAtOnceAndShrinksWhenStatic)
{
	auto *s = fill(1000, 0, 10, [](double, int) { return 7.0; });
	assign({s});
	const double top0 = chart_->visibleYMax();
	EXPECT_LE(top0, 25);
	// Slow frames over a lot of the window: the top grows immediately.
	for (int i = 0; i < 1000; ++i)
		s->append(10.0 + i * 0.01, 60.0);
	s->notify();
	EXPECT_GT(chart_->visibleYMax(), 60);
	// Static view onto only the steady part: shrinks back.
	chart_->setXMin(0);
	chart_->setXMax(9);
	EXPECT_EQ(chart_->visibleYMax(), top0);
}

TEST_F(ChartTest, FixedYMaxIsHonored)
{
	auto *s = fill(1000, 0, 10, [](double, int) { return 500.0; });
	assign({s});
	chart_->setYMax(33);
	EXPECT_EQ(chart_->visibleYMax(), 33);
	EXPECT_NEAR(chart_->yToPixel(33), 0, 1e-9);
	chart_->setYMax(0);
	EXPECT_GE(chart_->visibleYMax(), 500);
}

TEST_F(ChartTest, MappingRoundTrips)
{
	auto *s = fill(100, 2, 12, [](double, int) { return 8.0; });
	assign({s});
	chart_->setYMax(40);
	EXPECT_NEAR(chart_->xToPixel(chart_->visibleXMin()), 0, 1e-9);
	EXPECT_NEAR(chart_->xToPixel(chart_->visibleXMax()), kW, 1e-9);
	EXPECT_NEAR(chart_->yToPixel(0), kH, 1e-9);
	for (double x : {2.0, 3.7, 12.0})
		EXPECT_NEAR(chart_->pixelToX(chart_->xToPixel(x)), x, 1e-9);
	for (double y : {0.0, 8.0, 40.0})
		EXPECT_NEAR(chart_->pixelToY(chart_->yToPixel(y)), y, 1e-9);
}

TEST_F(ChartTest, MillionPointsStayBoundedAndFast)
{
	const int n = 1000000;
	auto *s = fill(n, 0, 3600, [](double, int i) { return 6.0 + (i % 97) * 0.05 + ((i % 100003) == 0 ? 80.0 : 0.0); });
	QElapsedTimer t;
	t.start();
	assign({s});
	chart_->setYMax(0);
	const qint64 rangeMs = t.elapsed();
	const QImage img = render();
	const qint64 paintNs = chart_->lastPaintNanoseconds();
	std::printf("[chart] 1M points: range recompute %lld ms, updatePaintNode %.2f ms, %d vertices\n",
	            (long long)rangeMs, double(paintNs) / 1e6, chart_->lastVertexCount());
	EXPECT_LE(chart_->lastVertexCount(), 16 * kW);
	EXPECT_GT(chart_->lastVertexCount(), 0);
	EXPECT_LT(paintNs, 300'000'000);
	EXPECT_LT(rangeMs, 300);
	EXPECT_GT(count(img, kRed, 0, kW - 1, 0, kH - 1), kW);

	// Zoomed to a slice found by binary search: cheap.
	chart_->setXMin(1000);
	chart_->setXMax(1010);
	render();
	EXPECT_LT(chart_->lastPaintNanoseconds(), 50'000'000);
}

TEST_F(ChartTest, DeletedSeriesIsHarmless)
{
	auto *a = fill(500, 0, 10, [](double, int) { return 5.0; });
	auto *b = fill(500, 0, 10, [](double, int) { return 9.0; });
	assign({a, b});
	chart_->setYMax(20);
	render();
	owned_.removeOne(b);
	delete b;
	EXPECT_EQ(chart_->series().size(), 1);
	const QImage img = render();
	EXPECT_EQ(count(img, kGreen, 0, kW - 1, 0, kH - 1), 0);
	EXPECT_GT(count(img, kRed, 0, kW - 1, 0, kH - 1), kW);
	// Replacing the assigned list disconnects the old series: its signals no longer reach the chart.
	assign({});
	int emitted = 0;
	QObject ctx;   // declared after `emitted`: the connection dies before the counter does
	QObject::connect(chart_, &FrameTimeChart::rangeChanged, &ctx, [&] { ++emitted; });
	a->append(11, 5);
	a->notify();
	EXPECT_EQ(emitted, 0);
	render();
}

// Every vertex of the series nodes must lie inside the item (no clip is set on the item).
static void expectSeriesInsideItem(QSGNode *root, int w, int h)
{
	int idx = 0;
	for (QSGNode *ch = root->firstChild(); ch; ch = ch->nextSibling(), ++idx)
	{
		if (idx < 2)
			continue;   // grid, reference
		const QSGGeometry *g = static_cast<QSGGeometryNode *>(ch)->geometry();
		const QSGGeometry::Point2D *v = g->vertexDataAsPoint2D();
		for (int i = 0; i < g->vertexCount(); ++i)
		{
			ASSERT_GE(v[i].x, 0.0f);
			ASSERT_LE(v[i].x, float(w));
			ASSERT_GE(v[i].y, 0.0f);
			ASSERT_LE(v[i].y, float(h));
		}
	}
}

TEST_F(ChartTest, LinesStayInsideTheItemAndSpikesClipAtTop)
{
	auto *s = fill(3000, 0, 10, [](double, int i) { return (i == 500 || i == 1500) ? 5000.0 : (i % 2 ? 0.0 : 7.0); });
	assign({s});
	chart_->setYMax(20);
	const QImage img = render();
	expectSeriesInsideItem(root_, kW, kH);
	EXPECT_GT(count(img, kRed, 0, kW - 1, 0, 3), 0);
}

TEST_F(ChartTest, LineContinuesToItemEdgesFromOutOfRangeNeighbours)
{
	// The visible window lies strictly between two points: the line is interpolated edge to edge.
	auto *s = makeSeries();
	s->setPoints({QPointF(0, 10), QPointF(0.4, 10)});
	assign({s});
	chart_->setYMax(20);
	chart_->setXMin(0.1);
	chart_->setXMax(0.3);
	const int y = int(std::lround(chart_->yToPixel(10)));
	QImage img = render();
	EXPECT_GT(count(img, kRed, 0, 1, y - 2, y + 2), 0);
	EXPECT_GT(count(img, kRed, kW - 2, kW - 1, y - 2, y + 2), 0);
	EXPECT_GT(count(img, kRed, 100, kW - 100, y - 2, y + 2), kW);

	// The window starts and ends mid-series, between samples.
	auto *d = fill(12, 0, 1.0, [](double, int) { return 10.0; });
	assign({d});
	chart_->setXMin(0.05);
	chart_->setXMax(0.95);
	img = render();
	EXPECT_GT(count(img, kRed, 0, 1, y - 2, y + 2), 0);
	EXPECT_GT(count(img, kRed, kW - 2, kW - 1, y - 2, y + 2), 0);
	expectSeriesInsideItem(root_, kW, kH);
}

TEST_F(ChartTest, ColorsCycleOverSeries)
{
	auto *a = fill(10, 0, 1, [](double, int) { return 5.0; });
	auto *b = fill(10, 0, 1, [](double, int) { return 6.0; });
	auto *c = fill(10, 0, 1, [](double, int) { return 7.0; });
	assign({a, b, c});
	render();
	QList<QColor> cols;
	int idx = 0;
	for (QSGNode *ch = root_->firstChild(); ch; ch = ch->nextSibling(), ++idx)
		if (idx >= 2)
			cols << static_cast<QSGFlatColorMaterial *>(static_cast<QSGGeometryNode *>(ch)->material())->color();
	ASSERT_EQ(cols.size(), 3);
	EXPECT_EQ(cols[0], kRed);
	EXPECT_EQ(cols[1], kGreen);
	EXPECT_EQ(cols[2], kRed);
}

TEST_F(ChartTest, HitchLongerThanTheGapThresholdStaysConnected)
{
	// 60 Hz with one 600 ms frame: the long frame's sample is 0.6 s after the previous one, but
	// the line must run up to it (and back down), not leave an isolated dot.
	QList<QPointF> pts;
	double x = 0;
	for (int i = 0; i < 600; ++i)
	{
		const bool hitch = i == 300;
		x += hitch ? 0.6 : 1.0 / 60;
		pts.append(QPointF(x, hitch ? 600.0 : 16.667));
	}
	auto *s = makeSeries();
	s->setPoints(pts);
	assign({s});
	chart_->setYMax(700);
	chart_->setXMin(0);
	chart_->setXMax(11);
	const QImage img = render();
	const int a = int(chart_->xToPixel(pts[299].x())), b = int(chart_->xToPixel(pts[300].x()));
	ASSERT_GT(b - a, 20);
	for (int col = a + 2; col < b - 2; ++col)
		EXPECT_GT(count(img, kRed, col, col, 0, kH - 1), 0) << "column " << col;
}

TEST_F(ChartTest, TinyItemHeightDoesNotMisbehave)
{
	auto *s = fill(100, 0, 10, [](double, int) { return 5.0; });
	assign({s});
	chart_->setReferenceLines({5});
	for (double h : {0.3, 0.9, 1.0})
	{
		chart_->setSize(QSizeF(kW, h));
		render();
		expectSeriesInsideItem(root_, kW, int(std::ceil(h)));
	}
}

TEST_F(ChartTest, LiveViewShrinksTheAutoTopOnlyAfterADelay)
{
	chart_->setFollow(true);
	chart_->setWindowSeconds(10);
	auto steady = [&](double v) {
		auto *s = makeSeries();
		QList<QPointF> pts;
		for (int i = 0; i < 500; ++i)
			pts.append(QPointF(i * 0.02, v));
		s->setPoints(pts);
		return s;
	};
	auto *s = steady(60);
	assign({s});
	const double big = chart_->visibleYMax();
	EXPECT_GE(big, 60);

	// Needed value drops far below the top: the top stays for now ...
	QList<QPointF> low;
	for (int i = 0; i < 500; ++i)
		low.append(QPointF(i * 0.02, 5));
	s->setPoints(low);
	s->notify();
	EXPECT_EQ(chart_->visibleYMax(), big);
	pmlguitest::waitUntil([] { return false; }, 700);
	EXPECT_EQ(chart_->visibleYMax(), big);
	// ... and shrinks once by itself (timer re-evaluation, no new data) after the delay.
	int emitted = 0;
	QObject ctx;   // declared after `emitted`: the connection dies before the counter does
	QObject::connect(chart_, &FrameTimeChart::rangeChanged, &ctx, [&] { ++emitted; });
	EXPECT_TRUE(pmlguitest::waitUntil([&] { return chart_->visibleYMax() < big; }, 3000));
	EXPECT_EQ(emitted, 1);
	EXPECT_LE(chart_->visibleYMax(), 10);

	// A dip that stays within the 0.7 band (needed 20 vs top 25) never shrinks the top.
	auto *t = steady(15);
	assign({t});
	const double top = chart_->visibleYMax();
	EXPECT_EQ(top, 25);
	QList<QPointF> mid;
	for (int i = 0; i < 500; ++i)
		mid.append(QPointF(i * 0.02, 11));
	t->setPoints(mid);
	t->notify();
	pmlguitest::waitUntil([] { return false; }, 1800);
	EXPECT_EQ(chart_->visibleYMax(), top);
}

// The software backend (the offscreen platform, QT_QUICK_BACKEND=software, machines without a GPU
// driver) cannot draw custom geometry; the chart then paints the same lines into an image node, so a
// real grabWindow() shows them (the GUI smoke test's screenshots rely on this).
TEST_F(ChartTest, SoftwareBackendShowsTheLinesInARealGrab)
{
	auto *s = fill(600, 0, 10, [](double, int) { return 10.0; });
	assign({s});
	chart_->setYMax(20);
	window_->show();
	ASSERT_TRUE(pmlguitest::waitUntil([&] { return window_->isExposed(); }));
	QImage img = window_->grabWindow();   // a frame through the window's own scene graph
	ASSERT_FALSE(img.isNull());
	img = img.convertToFormat(QImage::Format_ARGB32);
	ASSERT_EQ(img.width(), kW);
	const int y = int(std::lround(chart_->yToPixel(10)));
	EXPECT_GT(count(img, kRed, 50, kW - 50, y - 2, y + 2), (kW - 100) / 2);   // the line, along its length
	EXPECT_EQ(count(img, kRed, 0, kW - 1, 0, y - 6), 0);                      // and nothing far above it
	EXPECT_GT(chart_->lastVertexCount(), 0);

	// Later frames reuse the image node with a new texture each time (a double free of the old
	// texture once crashed the app from the second frame on). The line follows the data.
	for (int frame = 0; frame < 5; ++frame)
	{
		QList<QPointF> pts;
		for (int i = 0; i < 600; ++i)
			pts.append(QPointF(i / 60.0, 15.0));
		s->setPoints(std::move(pts));
		s->notify();
		img = window_->grabWindow().convertToFormat(QImage::Format_ARGB32);
		ASSERT_FALSE(img.isNull());
	}
	const int y15 = int(std::lround(chart_->yToPixel(15)));
	EXPECT_GT(count(img, kRed, 50, kW - 50, y15 - 2, y15 + 2), (kW - 100) / 2);
	EXPECT_EQ(count(img, kRed, 50, kW - 50, y - 2, y + 2), 0);   // the old line is gone
}
