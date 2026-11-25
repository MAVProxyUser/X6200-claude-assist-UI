"""CISA's NIFOG on the radio (/usr/share/x6100/nifog, from the image and the update bundle): its contents for the page
/nifog. The index is tools/nifog/nifog.idx (tools/nifog/render.py --index), the GUI's too (src/nifog.c):

    edition 2.02, January 2025
    pages 192
    page P LABEL             PDF page P is printed LABEL (31, iv)
    sec P LEVEL TITLE        the contents' entry on PDF page P: level 0 a chapter, 1 a section, 2 a part of one
"""
import os


def read_index(folder):
    """{edition, pages, labels {page: label}, secs [{page, level, title}], pdf_size}; pages 0 when there is none"""
    out = {"edition": "", "pages": 0, "labels": {}, "secs": [], "pdf_size": 0}
    try:
        out["pdf_size"] = os.path.getsize(os.path.join(folder, "nifog.pdf"))
    except OSError:
        pass
    try:
        with open(os.path.join(folder, "nifog.idx"), encoding="ascii", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        return out
    for line in lines:
        kind, _, rest = line.partition(" ")
        if kind == "edition":
            out["edition"] = rest.strip()
        elif kind == "pages" and rest.strip().isdigit():
            out["pages"] = int(rest)
        elif kind == "page":
            p, _, label = rest.partition(" ")
            if p.isdigit():
                out["labels"][int(p)] = label.strip()
        elif kind == "sec":
            p, _, rest = rest.partition(" ")
            lv, _, title = rest.partition(" ")
            if p.isdigit() and lv.isdigit() and title.strip():
                out["secs"].append({"page": int(p), "level": int(lv), "title": title.strip()})
    out["secs"] = [s for s in out["secs"] if 1 <= s["page"] <= out["pages"]]
    return out


def page_file(folder, n, pages):
    """The PNG of page n (1 the first), None when there is no such page"""
    if not 1 <= n <= pages:
        return None
    name = "p%03d.png" % n
    return name if os.path.isfile(os.path.join(folder, name)) else None
