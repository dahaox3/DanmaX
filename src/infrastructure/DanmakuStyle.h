#pragma once
#include <QVariantMap>

// Source appearance is a rendering policy; never rewrite XML, caches or records.
inline QVariantMap danmakuStyle(QVariantMap values, bool external, bool localXml) {
    if (external && localXml) {
        for (const auto* key : {"FontFamily", "FontSize", "StrokeWidth", "Opacity"}) {
            auto normal = QString::fromLatin1(key); normal[0] = normal[0].toLower();
            values[normal] = values.value("localXml" + QString::fromLatin1(key));
        }
        values["fontWeight"] = values.value("localXmlBold").toBool() ? 700 : 400;
        values["uniformFontSize"] = true;
        values["textColor"] = values.value("localXmlUseSourceColor").toBool() ? QString() : values.value("localXmlColor").toString();
    } else if (external) {
        // The API supplies size/color/mode, but no font family/weight/outline.
        // Fields absent from the source use the general preferences.
        values["fontSize"] = 25;
    }
    return values;
}
