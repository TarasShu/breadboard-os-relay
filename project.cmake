# PROJECT NAME - in quotes, no spaces
set(PROJ_NAME "my-bbos-proj")

# PROJECT VERSION - in quotes, no spaces, can contain alphanumeric if necessary
set(PROJ_VER "0.0")

# CLI INTERFACE - 0: use UART for CLI (default), 1: use USB for CLI
set(CLI_IFACE 1)

# MCU PLATFORM - set the MCU platform being used (i.e. the subdir in 'hardware/')
set(PLATFORM rp2xxx)

# BOARD - set the board being used (platform-specific prebuild.cmake contains more information about boards)
set(BOARD pico_w)

# HOSTNAME - hostname will be shown at CLI prompt, and used for network connections
set(HOSTNAME "bbos")

# BUILD OPTIONS - individual features which can be enabled or disabled
option(ENABLE_MOTD "Enable Message of the Day print at boot" true)
option(ENABLE_WIFI "Enable WiFi support" true)
option(ENABLE_HTTPD "Enable httpd web server" true)
