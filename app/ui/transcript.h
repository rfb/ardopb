#ifndef ARDOP_UI_TRANSCRIPT_H_
#define ARDOP_UI_TRANSCRIPT_H_

#include <QCheckBox>
#include <QPlainTextEdit>
#include <QString>
#include <QWidget>

/**
 * @file transcript.h
 * @brief A timestamped, bounded, escaped log of things said in both directions.
 *
 * Three screens want the same widget -- the Console shows commands and replies,
 * the Chat shows messages, the Files screen shows transfers -- and the first two
 * of those were written separately. This is the one implementation.
 *
 * ## Escaping is the reason this is a class and not a helper
 *
 * The Console rendered every received line as a bare timestamp with its content
 * missing, because the direction marker `<<` is both a perfectly good arrow and
 * a perfectly good start of an HTML tag, and `appendHtml` believed the second
 * reading. That was a cosmetic bug in a screen whose text comes from our own
 * modem.
 *
 * **The Chat screen's text comes from a stranger over the radio**, and the same
 * mistake there is a peer choosing what this station's operator sees -- markup,
 * a hyperlink, an image reference. So escaping is not left to the caller: this
 * class takes plain text and marked-up text is impossible to pass in.
 *
 * It is tested in `test_widgets.cpp` for exactly that, because it is the kind of
 * property that is invisible until the day somebody exercises it.
 *
 * ## Follow, and why the scroll position is restored by hand
 *
 * A transcript that yanks itself to the bottom while someone is reading back
 * through it is one they cannot read back through -- and traffic keeps arriving
 * while they read. The checkbox is part of the widget rather than of each screen
 * so that every transcript behaves the same way.
 */
class Transcript : public QWidget {
	Q_OBJECT

public:
	/**
	 * @param monospace Fixed-pitch, for anything with alignment or protocol
	 *                  text in it. Chat is prose and reads better without.
	 */
	explicit Transcript(bool monospace = true, QWidget *parent = nullptr);

	/**
	 * @brief Append one line.
	 *
	 * @param prefix A short direction marker, e.g. `>>`. Escaped, and its
	 *               spaces are preserved so markers of different widths line
	 *               up.
	 * @param text   Plain text. Any markup in it is shown, not obeyed.
	 * @param colour A CSS colour for the line, or nullptr for the default.
	 */
	void append(const QString &prefix, const QString &text,
		    const char *colour = nullptr);

	/** @brief The plain text of everything appended. For tests. */
	QString plainText() const { return m_view->toPlainText(); }

	/** @brief Show the Follow checkbox. On by default. */
	void setFollowVisible(bool on);

	void clear() { m_view->clear(); }

private:
	QPlainTextEdit *m_view = nullptr;
	QCheckBox *m_follow = nullptr;
};

#endif /* ARDOP_UI_TRANSCRIPT_H_ */
