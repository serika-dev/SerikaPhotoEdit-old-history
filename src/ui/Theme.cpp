#include "Theme.h"
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>

namespace serika {
namespace {
QString headingFamily = QStringLiteral("Outfit");
QString bodyFamily = QStringLiteral("Onest");
struct ThemeColors {
    const char *window;
    const char *panel;
    const char *base;
    const char *text;
    const char *muted;
    const char *border;
    const char *control;
    const char *accent;
    const char *accentText;
    const char *onAccent;
    const char *soft;
};
ThemeColors colors(const QString &name) {
    if (name == "Darkest")
        return {"#0E0B15", "#181421", "#0A0A0A", "#F4F1FB", "#A098B4", "#2D273C",
                "#796C8F", "#8B5CF6", "#C4B5FD", "#0A0A0A", "#29214A"};
    if (name == "Light")
        return {"#EEEBF6", "#FFFFFF", "#FAF7FF", "#17131F", "#6F6880", "#D8D1E5",
                "#8A7A9E", "#5B21B6", "#5B21B6", "#FAFAFA", "#ECE7FF"};
    if (name == "Lightest")
        return {"#F5F3FA", "#FFFFFF", "#FFFFFF", "#17131F", "#6F6880", "#E5E1EF",
                "#8A7A9E", "#5B21B6", "#5B21B6", "#FAFAFA", "#ECE7FF"};
    return {"#181421", "#221D2E", "#0E0B15", "#F4F1FB", "#A098B4", "#2D273C",
            "#81728F", "#8B5CF6", "#C4B5FD", "#0A0A0A", "#29214A"};
}
QString loadedFamily(const QString &path, const QString &preferred) {
    const int id = QFontDatabase::addApplicationFont(path);
    if (id < 0)
        return preferred;
    const QStringList families = QFontDatabase::applicationFontFamilies(id);
    if (families.contains(preferred))
        return preferred;
    return families.isEmpty() ? preferred : families.first();
}
} // namespace
void initializeBrandFonts() {
    static bool initialized = false;
    if (initialized || !QGuiApplication::instance())
        return;
    initialized = true;
    headingFamily = loadedFamily(":/serika/fonts/Outfit.ttf", "Outfit");
    bodyFamily = loadedFamily(":/serika/fonts/Onest.ttf", "Onest");
}
QColor themeCanvas(const QString &name) {
    return name == "Darkest"    ? QColor("#0A0A0A")
           : name == "Light"    ? QColor("#ADADAD")
           : name == "Lightest" ? QColor("#D6D6D6")
                                : QColor("#222222");
}
QPalette themePalette(const QString &name) {
    const ThemeColors c = colors(name);
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(c.window));
    palette.setColor(QPalette::WindowText, QColor(c.text));
    palette.setColor(QPalette::Base, QColor(c.base));
    palette.setColor(QPalette::AlternateBase, QColor(c.panel));
    palette.setColor(QPalette::ToolTipBase, QColor(c.panel));
    palette.setColor(QPalette::ToolTipText, QColor(c.text));
    palette.setColor(QPalette::Text, QColor(c.text));
    palette.setColor(QPalette::Button, QColor(c.panel));
    palette.setColor(QPalette::ButtonText, QColor(c.text));
    palette.setColor(QPalette::BrightText, QColor("#FAFAFA"));
    palette.setColor(QPalette::Highlight, QColor(c.accent));
    palette.setColor(QPalette::Accent, QColor(c.accent));
    palette.setColor(QPalette::HighlightedText, QColor(c.onAccent));
    palette.setColor(QPalette::Link, QColor(c.accentText));
    palette.setColor(QPalette::LinkVisited, QColor(c.accentText));
    palette.setColor(QPalette::PlaceholderText, QColor(c.muted));
    palette.setColor(QPalette::Light, QColor(c.panel).lighter(125));
    palette.setColor(QPalette::Midlight, QColor(c.soft));
    palette.setColor(QPalette::Mid, QColor(c.control));
    palette.setColor(QPalette::Dark, QColor(c.border));
    palette.setColor(QPalette::Shadow, themeCanvas(name));
    for (const auto role :
         {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::PlaceholderText})
        palette.setColor(QPalette::Disabled, role, QColor(c.muted));
    palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(c.soft));
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor(c.muted));
    return palette;
}
QString themeStyle(const QString &name) {
    initializeBrandFonts();
    const QString theme = name == "Darkest" || name == "Light" || name == "Lightest" ? name : "Dark";
    QFile file(":/serika/themes/" + theme.toLower() + ".qss");
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QString style = QString::fromUtf8(file.readAll());
    // Settings-style surfaces shared by every theme, written against its colour roles.
    const ThemeColors c = colors(theme);
    const bool light = theme.startsWith("Light");
    QString shared = R"(
QWidget#prefsSidebar { background: @panel; border-right: 1px solid @border; }
QLabel#prefsBrand { background: transparent; color: @muted; font-family: 'Outfit'; font-size: 11px; font-weight: 600; letter-spacing: 1px; padding: 0 4px; }
QLineEdit#prefsSearch { background: @base; border: 1px solid @border; border-radius: 7px; padding: 6px 9px; font-size: 12px; }
QLineEdit#prefsSearch:focus { border-color: @accent; }
QListWidget#prefsNav { background: transparent; border: 0; font-size: 12px; }
QListWidget#prefsNav::item { color: @text; padding: 7px 8px; margin: 1px 0; border: 0; border-radius: 7px; }
QListWidget#prefsNav::item:hover { background: @soft; }
QListWidget#prefsNav::item:selected { background: @soft; color: @accentText; border-left: 3px solid @accent; }
QListWidget#prefsNav::item:focus { border: 0; }
QListWidget#prefsNav::item:selected:focus { border-left: 3px solid @accent; }
QScrollArea#prefsScroll, QWidget#prefsPage { background: @window; border: 0; }
QLabel#prefsTitle { background: transparent; font-family: 'Outfit'; font-size: 22px; font-weight: 600; }
QLabel#prefsSubtitle { background: transparent; color: @muted; font-size: 12px; }
QFrame#prefsCard { background: @panel; border: 1px solid @border; border-radius: 10px; }
QLabel#prefsCardTitle { background: transparent; color: @muted; font-family: 'Outfit'; font-size: 11px; font-weight: 600; letter-spacing: 1px; padding: 2px 2px 0 2px; }
QFrame#prefsRow { background: transparent; border: 0; border-top: 1px solid @border; }
QFrame#prefsRow[first="true"] { border-top: 0; }
QWidget#prefsBare, QFrame#prefsCard QLabel, QFrame#prefsCard QCheckBox, QFrame#prefsCard QRadioButton, QFrame#prefsCard QSlider { background: transparent; }
QLabel#prefsLabel { font-size: 12px; }
QLabel#prefsHint { color: @muted; font-size: 11px; }
QLabel#prefsEmpty { background: transparent; color: @muted; font-size: 13px; }
QLabel#prefsBadge { border-radius: 9px; padding: 2px 9px; font-weight: 600; font-size: 11px; }
QLabel#prefsBadge[state="ok"] { background: @okSoft; color: @ok; }
QLabel#prefsBadge[state="warn"] { background: @warnSoft; color: @warn; }
QLabel#prefsBadge[state="off"] { background: @window; color: @muted; border: 1px solid @border; }
QFrame#prefsChoice { background: @window; border: 1px solid @border; border-radius: 8px; }
QFrame#prefsChoice:hover { border-color: @control; }
QFrame#prefsChoice[checked="true"] { background: @soft; border: 1px solid @accent; }
QFrame#prefsChoice QLabel { background: transparent; }
QFrame#prefsFooter { background: @panel; border: 0; border-top: 1px solid @border; }
)";
    const QList<QPair<QString, QString>> roles{{"@window", c.window},
                                               {"@panel", c.panel},
                                               {"@base", c.base},
                                               {"@text", c.text},
                                               {"@muted", c.muted},
                                               {"@border", c.border},
                                               {"@control", c.control},
                                               {"@accentText", c.accentText},
                                               {"@accent", c.accent},
                                               {"@soft", c.soft},
                                               {"@okSoft", light ? "#DCFCE7" : "rgba(34, 197, 94, 0.16)"},
                                               {"@ok", light ? "#15803D" : "#4ADE80"},
                                               {"@warnSoft", light ? "#FEF3C7" : "rgba(245, 158, 11, 0.16)"},
                                               {"@warn", light ? "#B45309" : "#FBBF24"}};
    for (const auto &[token, value] : roles)
        shared.replace(token, value);
    style += shared;
    style.replace("'Outfit'", "'" + headingFamily + "'");
    style.replace("'Onest'", "'" + bodyFamily + "'");
    return style;
}
} // namespace serika
