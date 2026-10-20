#pragma once
// DashboardPages — how the dashboard's pages and their layouts are stored.
//
// Settings keys: the first page ("main") keeps the original single-dashboard
// key `dashboard_canvas_layout`, so an existing layout opens unchanged as page 1
// (and an older build still reads it); every other page is stored under
// `dashboard_canvas_layout:<id>`, and the page list with the current page under
// `dashboard_pages`. Header-only, Qt Core only, so the rules are unit-testable.
#include "screens/dashboard/canvas/GridLayout.h"

#include <QByteArray>
#include <QDataStream>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QVector>

#include <algorithm>

namespace fincept::screens::pages {

inline constexpr const char* kPagesKey = "dashboard_pages";
inline constexpr const char* kFirstPageId = "main";

// Magic + version prefix for the serialized dashboard layout blob. Distinguishes
// the current config-carrying format from the legacy one (which packed items
// with no per-item config). `cols` (a small int) never collides with the magic,
// so legacy blobs are still decoded (with empty per-item config).
inline constexpr quint32 kDashLayoutMagic = 0xDA58A101u;
inline constexpr quint32 kDashLayoutVersion = 1;

inline QString layout_key(const QString& page_id) {
    return page_id == QLatin1String(kFirstPageId) ? QStringLiteral("dashboard_canvas_layout")
                                                  : QStringLiteral("dashboard_canvas_layout:") + page_id;
}

inline QString encode_layout(const GridLayout& layout) {
    // Serialize: magic+version, cols, row_h, margin, item count, then each item
    // (including its per-instance config — dropping it reverted every configured
    // widget to defaults on restart).
    QByteArray buf;
    QDataStream stream(&buf, QIODevice::WriteOnly);
    stream << kDashLayoutMagic << kDashLayoutVersion;
    stream << layout.cols << layout.row_h << layout.margin;
    stream << static_cast<int>(layout.items.size());
    for (const auto& item : layout.items) {
        stream << item.id << item.instance_id;
        stream << item.cell.x << item.cell.y << item.cell.w << item.cell.h;
        stream << item.cell.min_w << item.cell.min_h;
        stream << QJsonDocument(item.config).toJson(QJsonDocument::Compact);
    }
    return QString::fromLatin1(buf.toBase64());
}

/// Decode a stored layout; false for an absent, empty or corrupt blob.
inline bool decode_layout(const QString& encoded, GridLayout& layout) {
    if (encoded.isEmpty())
        return false;
    QByteArray payload = QByteArray::fromBase64(encoded.toUtf8());
    QDataStream stream(&payload, QIODevice::ReadOnly);

    int count = 0;
    // Detect the format: a leading magic means the config-carrying
    // blob; otherwise the 4 bytes we read were the legacy `cols`.
    quint32 lead = 0;
    stream >> lead;
    const bool has_config = (lead == kDashLayoutMagic);
    if (has_config) {
        quint32 version = 0;
        stream >> version; // reserved for future migrations
        stream >> layout.cols >> layout.row_h >> layout.margin >> count;
    } else {
        layout.cols = static_cast<int>(lead);
        stream >> layout.row_h >> layout.margin >> count;
    }
    if (count <= 0 || count > 100)
        return false;

    // Clamp the grid geometry before anything downstream uses it.
    // A truncated / corrupted blob previously produced items with
    // cols == 0 or w == 0, which grid_to_rect() turns into
    // zero-width tiles and compact_vertical() spins over.
    layout.cols = qBound(1, layout.cols, 48);
    layout.row_h = qBound(10, layout.row_h, 400);
    layout.margin = qBound(0, layout.margin, 64);

    for (int i = 0; i < count; ++i) {
        GridItem item;
        stream >> item.id >> item.instance_id;
        stream >> item.cell.x >> item.cell.y >> item.cell.w >> item.cell.h;
        stream >> item.cell.min_w >> item.cell.min_h;
        if (has_config) {
            QByteArray cfg_bytes;
            stream >> cfg_bytes;
            const auto doc = QJsonDocument::fromJson(cfg_bytes);
            if (doc.isObject())
                item.config = doc.object();
        }
        // Stop at the first short read rather than appending
        // default-constructed garbage for the remaining items.
        if (stream.status() != QDataStream::Ok)
            break;
        if (item.id.isEmpty() || item.instance_id.isEmpty())
            continue;
        item.cell.min_w = qBound(1, item.cell.min_w, layout.cols);
        item.cell.min_h = qBound(1, item.cell.min_h, 64);
        item.cell.w = qBound(item.cell.min_w, item.cell.w, layout.cols);
        item.cell.h = qBound(item.cell.min_h, item.cell.h, 64);
        item.cell.x = qBound(0, item.cell.x, layout.cols - item.cell.w);
        item.cell.y = qBound(0, item.cell.y, 999);
        layout.items.append(item);
    }
    return !layout.items.isEmpty();
}

struct PageRef {
    QString id;
    QString name;
};

/// The stored page list. Unknown, duplicate or nameless entries are cleaned
/// up and the first page is always present (first). `current_id` receives the
/// stored current page (the first page when none is stored).
inline QVector<PageRef> parse_pages(const QString& stored, const QString& first_name, QString* current_id) {
    QVector<PageRef> list;
    QString current = QString::fromLatin1(kFirstPageId);
    if (!stored.isEmpty()) {
        const QJsonObject doc = QJsonDocument::fromJson(stored.toUtf8()).object();
        for (const auto& v : doc.value(QStringLiteral("pages")).toArray()) {
            const QJsonObject o = v.toObject();
            const QString id = o.value(QStringLiteral("id")).toString();
            if (id.isEmpty() || std::any_of(list.cbegin(), list.cend(), [&id](const PageRef& p) { return p.id == id; }))
                continue;
            list.append({id, o.value(QStringLiteral("name")).toString()});
        }
        current = doc.value(QStringLiteral("current")).toString(current);
    }
    const auto first =
        std::find_if(list.begin(), list.end(), [](const PageRef& p) { return p.id == QLatin1String(kFirstPageId); });
    if (first == list.end()) {
        list.prepend({QString::fromLatin1(kFirstPageId), first_name});
    } else if (first != list.begin()) {
        const PageRef main = *first;
        list.erase(first);
        list.prepend(main);
    }
    if (std::none_of(list.cbegin(), list.cend(), [&current](const PageRef& p) { return p.id == current; }))
        current = QString::fromLatin1(kFirstPageId);
    if (current_id)
        *current_id = current;
    return list;
}

inline QString pages_json(const QVector<PageRef>& list, const QString& current_id) {
    QJsonArray arr;
    for (const auto& p : list) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), p.id);
        o.insert(QStringLiteral("name"), p.name);
        arr.append(o);
    }
    QJsonObject doc;
    doc.insert(QStringLiteral("pages"), arr);
    doc.insert(QStringLiteral("current"), current_id);
    return QString::fromUtf8(QJsonDocument(doc).toJson(QJsonDocument::Compact));
}

/// The page to show after page `deleted` is removed while page `current` was
/// shown (indices before removal): the same page stays shown; when the shown
/// page itself is deleted, its neighbour (the next, or the last) is shown.
inline int current_after_delete(int deleted, int current, int count_after) {
    if (count_after <= 0)
        return -1;
    if (deleted < current)
        return current - 1;
    if (deleted > current)
        return current;
    return std::min(deleted, count_after - 1);
}

} // namespace fincept::screens::pages
