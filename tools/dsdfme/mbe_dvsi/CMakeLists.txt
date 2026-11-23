# libmbe-dvsi as the GUI's "mbe" (src/dmr_ambe.c, upstream's DMR app): its vocoder calls answered by the AMBE-3000
# behind the SDR daemon's port (127.0.0.1:2460, DSD_DVSI), never by mbelib. The owner (2026-11-16): no mbelib ever,
# DVSI hardware only; so upstream's third-party/mbelib is not in this fork, and with no USB-3000 the app has no voice.
add_library(mbe STATIC mbe_dvsi.c)
target_include_directories(mbe PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mbe PUBLIC m)
