# Transport-neutral app-layer driver sources: everything under app/src except
# the USB application main (app.cpp) and the USB host transport (usb/).
#
# Host-link applications that reuse the shared drivers on another transport
# (the EtherCAT bridge core1) consume LIBRMCS_APP_DRIVER_SOURCES instead of
# hand-listing files, so a new driver source is picked up by every consumer.
# The USB application itself globs the whole app/src tree and does not need
# this list.
file(GLOB_RECURSE LIBRMCS_APP_DRIVER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/../app/src/*.cpp"
)
list(FILTER LIBRMCS_APP_DRIVER_SOURCES EXCLUDE REGEX "/app/src/(usb/|app\\.cpp)")
