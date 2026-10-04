"""Neighbour bounds for undated observations in ordered local CSV files."""


def uncertainty_windows(rows):
    """Return ordering and inclusive ISO-date windows; None means open-ended.

    Rows are (physical row number, parsed date or None), excluding empty rows.
    Equal dates are allowed (published buckets are grouped by reporting date).
    Unordered input cannot locate an undated observation. A single distinct
    dated neighbour cannot establish the direction of an open-ended window.
    """
    dated = [day for _, day in rows if day is not None]
    ascending = all(a <= b for a, b in zip(dated, dated[1:]))
    descending = all(a >= b for a, b in zip(dated, dated[1:]))
    ordered = ascending or descending
    windows = []
    following = [None] * len(rows)
    after = None
    for index in range(len(rows) - 1, -1, -1):
        following[index] = after
        if rows[index][1] is not None:
            after = rows[index][1]
    before = None
    for index, (number, day) in enumerate(rows):
        if day is not None:
            before = day
            continue
        after = following[index]
        if not ordered or (ascending and descending and (before is None or after is None)):
            lower, upper = None, None
        elif ascending:
            lower, upper = before, after
        else:
            lower, upper = after, before
        windows.append({"row": number, "older_date": lower.isoformat() if lower else None,
                        "newer_date": upper.isoformat() if upper else None,
                        "neighbour_dates": sorted({d.isoformat() for d in (before, after) if d is not None})})
    return ordered, windows
