/**
 * @file test_widgets.cpp
 * @brief What can be asserted about the instruments without a person looking.
 *
 * The instrument panel is judged by eye, which is why it has had no tests. But
 * one property is both mechanical and has already regressed once, silently, in a
 * way nobody developing on Linux could see: **the waterfall must blit its image
 * one row per device pixel.**
 *
 * When it does not, a new scan line lands on a different sub-pixel phase each
 * frame and the picture shimmers as it scrolls. The first fix sized the image to
 * the widget's height() -- which is in *logical* pixels, so it was exact at 100%
 * and fractional at the 125% and 150% that most Windows machines run at. It
 * looked fixed for months on a 100% display.
 *
 * So the test runs at four scale factors, in a child process each, because
 * Qt fixes the ratio when QGuiApplication is constructed and it cannot be
 * changed afterwards.
 *
 * Offscreen, so it needs no display and runs in CI.
 */

#include <QApplication>
#include <QImage>
#include <QProcess>
#include <QProcessEnvironment>

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>

#include "levelmeter.h"
#include "waterfallwidget.h"

namespace {

int failures = 0;

void check(bool ok, const char *what)
{
	printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok)
		failures++;
}

/*
 * Feed a single bright scan line among silence and count how many output rows
 * it covers on each frame as it scrolls.
 *
 * A one-to-one blit reproduces it at a constant thickness. A fractional vertical
 * scale alternates between one row and two -- a line that breathes as it moves,
 * which is exactly what the eye reads as shimmer. The count is the assertion
 * because it is what differs; the peak brightness stays 255 either way, since
 * the scale is nearest-neighbour and duplicates rows rather than blending them.
 */
void scan_line_thickness(void)
{
	WaterfallWidget w;
	w.resize(300, 216);
	w.setSpectrumGeometry(206, 0, 11.71875f);

	const qreal dpr = w.devicePixelRatioF();
	printf("device pixel ratio %.2f, canvas %d logical px\n",
	       dpr, w.height() - 16);

	int first = -1;
	bool constant = true;
	const int kMarkerFrame = 3;   /* after the floor has seen the baseline */

	for (int frame = 0; frame < 24; frame++) {
		/*
		 * A quiet baseline with exactly one bright line in it.
		 *
		 * The baseline is what the noise-floor tracker settles on, and it
		 * has to come first: the widget seeds the floor from the very
		 * first row it sees, so a marker on frame 0 would define the
		 * floor as itself and paint black. One marker, so what is
		 * measured is the thickness of a line and not a count of lines.
		 */
		SpectrumRow row;
		row.mag.resize(206);
		for (int i = 0; i < 206; i++)
			row.mag[i] = (frame == kMarkerFrame) ? 1.0e6f : 1.0e2f;
		w.addRow(row);

		if (frame < kMarkerFrame)
			continue;

		QImage shot(w.size() * dpr, QImage::Format_RGB32);
		shot.setDevicePixelRatio(dpr);
		w.render(&shot);

		/* The longest run of lit pixels down the middle column. */
		const int x = shot.width() / 2;
		int run = 0, best = 0;
		for (int y = 0; y < shot.height() - int(16 * dpr); y++) {
			if (qGray(shot.pixel(x, y)) > 40) {
				if (++run > best)
					best = run;
			} else {
				run = 0;
			}
		}

		if (first < 0)
			first = best;
		else if (best != first)
			constant = false;
	}

	check(first == 1, "a one-row scan line renders as one row");
	check(constant, "and keeps that thickness on every frame as it scrolls");
}

/* The image must hold enough rows to fill the canvas in device pixels. Sizing it
 * to the logical height is the bug this file exists for, so assert the size
 * directly as well as its visible consequence. */
void image_covers_canvas(void)
{
	WaterfallWidget w;
	w.resize(300, 1200);   /* taller than kHistory, to force a grow */
	w.setSpectrumGeometry(206, 0, 11.71875f);

	SpectrumRow row;
	row.mag.resize(206);
	w.addRow(row);

	QImage shot(w.size() * w.devicePixelRatioF(), QImage::Format_RGB32);
	shot.setDevicePixelRatio(w.devicePixelRatioF());
	w.render(&shot);   /* paintEvent is where the image is grown */

	const int need = int((w.height() - 16) * w.devicePixelRatioF());
	check(w.historyRows() >= need,
	      "the image holds at least a canvas-worth of device rows");
}

/*
 * The meter's ballistics, which amendment 4 says belong here rather than in a
 * comment. Two properties, both mechanical:
 *
 *  1. the decay depends on elapsed time and not on how often setValue was
 *     called -- the reason the ballistics are on a repaint timer at all; and
 *  2. the colour is decided against a band, so "far too quiet" is not painted
 *     the same as "correct", which is the defect §9 exists to fix.
 */
void meter_decay_is_time_based(void)
{
	LevelMeter fast(QStringLiteral("a")), slow(QStringLiteral("b"));
	for (LevelMeter *m : {&fast, &slow}) {
		m->setScale(-60.0, 0.0);
		m->setBand(-24.0, -12.0);
		m->setValue(-10.0, -10.0);   /* attack to a known level */
		m->setValue(-50.0, -50.0);   /* then the signal drops away */
	}

	/* The same half second, delivered as one advance and as ten. A meter
	 * whose decay rode the data rate would land in two different places. */
	fast.advanceForTest(500);
	for (int i = 0; i < 10; i++)
		slow.advanceForTest(50);

	const double diff = std::fabs(fast.displayedLevel() - slow.displayedLevel());
	printf("    one 500 ms step: %.2f dB, ten 50 ms steps: %.2f dB\n",
	       fast.displayedLevel(), slow.displayedLevel());
	check(diff < 2.0, "decay follows elapsed time, not the update rate");

	/* And it is a decay, not a jump: after half a second the bar has left
	 * -10 but has not arrived at -50. */
	check(fast.displayedLevel() < -10.0 && fast.displayedLevel() > -50.0,
	      "the level decays rather than snapping to the new value");
}

void meter_band_colours_both_faults(void)
{
	LevelMeter m(QStringLiteral("rx"));
	m.resize(300, 40);
	m.setScale(-60.0, 0.0);
	m.setBand(-24.0, -12.0);

	/* Rendered rather than inspected, because the colour is what an operator
	 * sees and a getter could agree with the header while the paint did not.
	 * The sample is taken at the far left, which every non-empty bar covers. */
	auto barColourAt = [&m](double level) {
		/* Reset first: setValue only seeds the displayed level on the
		 * first reading, and after that the ballistics own it. Starting
		 * from "no data" makes each sample an instant attack, which is
		 * what isolates the colour from the decay. */
		m.setUnknown(QString());
		m.setValue(level, level);
		m.advanceForTest(0);
		QImage shot(m.size(), QImage::Format_RGB32);
		m.render(&shot);
		return shot.pixel(2, 10 + 8);   /* inside the trough */
	};

	const QRgb tooQuiet = barColourAt(-40.0);
	const QRgb correct = barColourAt(-18.0);
	const QRgb hot = barColourAt(-4.0);

	check(tooQuiet != correct,
	      "40 dB too quiet does not look the same as correct");
	check(hot != correct, "and neither does too loud");
	check(qGreen(correct) > qRed(correct) && qGreen(correct) > qBlue(correct),
	      "a reading inside the band is green");
}

}   // namespace

int main(int argc, char **argv)
{
	/*
	 * Re-exec once per scale factor. Qt reads QT_SCALE_FACTOR when
	 * QGuiApplication is constructed and the ratio is fixed from then on, so
	 * a single process cannot cover more than one.
	 */
	if (argc == 1) {
		/* QProcess rather than system(): the child needs two environment
		 * variables set, and `VAR=x prog` is shell syntax that cmd.exe
		 * does not have. */
		QCoreApplication parent(argc, argv);
		static const char *const kScales[] = {"1", "1.25", "1.5", "2"};
		int rc = 0;

		for (const char *scale : kScales) {
			printf("\n== QT_SCALE_FACTOR=%s ==\n", scale);
			fflush(stdout);

			QProcessEnvironment env =
				QProcessEnvironment::systemEnvironment();
			env.insert("QT_QPA_PLATFORM", "offscreen");
			env.insert("QT_SCALE_FACTOR", scale);

			QProcess child;
			child.setProcessEnvironment(env);
			child.setProcessChannelMode(QProcess::ForwardedChannels);
			child.start(QCoreApplication::applicationFilePath(),
				    {"--child"});
			if (!child.waitForFinished(60000) ||
			    child.exitStatus() != QProcess::NormalExit ||
			    child.exitCode() != 0)
				rc = 1;
		}
		printf("\n%s\n", rc ? "test_widgets: FAILED" : "test_widgets: ok");
		return rc;
	}

	QApplication app(argc, argv);
	scan_line_thickness();
	image_covers_canvas();
	meter_decay_is_time_based();
	meter_band_colours_both_faults();
	return failures ? 1 : 0;
}
