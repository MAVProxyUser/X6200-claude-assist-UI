# NIFOG 2.02

`NIFOG_2.02.pdf` is the **National Interoperability Field Operations Guide**, version 2.02 (January 2025), published
by the **Cybersecurity and Infrastructure Security Agency (CISA)**, U.S. Department of Homeland Security. It is
CISA's file **unchanged**, byte for byte, as downloaded from CISA on 2026-11-25:

- source: https://www.cisa.gov/sites/default/files/2024-12/NIFOG%202.02_508%20FINAL%20VERSION%2012%2003%202024.pdf
  (CISA's page for it: cisa.gov/safecom/field-operations-guides)
- 6,735,362 bytes, 192 pages
- SHA-256 `45c2f5d94861b3ed1b80f7ce5962a160fdd56092211586bdee711b68ca3d3142`

A work of the United States Government, not subject to copyright in the United States (17 U.S.C. 105). CISA's own
notice in it: names of commercial products and services may be trademarks of their respective owners, and their use
does not imply endorsement by CISA or the U.S. Government. Neither does its place in this firmware: GoldCorpHF is
not affiliated with CISA. Comments on the guide go to CISA (NIFOG@cisa.dhs.gov), not to this project.

On the radio (`/usr/share/x6100/nifog`, from the image and the update bundle):

- `nifog.pdf`, this file, served by the web configurator at `/nifog.pdf` and listed on its page `/nifog`;
- `p001.png` ... `p192.png`, its pages drawn at build time for the Sentry's F > NIFOG (the radio has no PDF
  renderer): `tools/nifog/render.py` with poppler's `pdftoppm`, 130 dpi, 16 greys, 5.2 MB;
- `nifog.idx`, its contents and its pages' printed numbers (`tools/nifog/nifog.idx`, made by
  `render.py --index` from the PDF's own contents).

A new edition: replace the PDF (its name in `CMakeLists.txt`), run
`python3 tools/nifog/render.py --pdf third-party/nifog/<new>.pdf --index > tools/nifog/nifog.idx`, update this file.
