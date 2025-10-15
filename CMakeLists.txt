cmake_minimum_required(VERSION 3.10)
project(cv2x_rpc_server)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Cap'n Proto
find_package(CapnProto REQUIRED)
capnp_generate_cpp(CAPNP_SRCS CAPNP_HDRS vanetza.capnp)

# Autotalks SDK Paths
set(AUTOTALKS_SDK_SRC /home/eee-rpi/cv2x_testing/secton-5.19.1-rel_sec_SPI0)
set(AUTOTALKS_CV2X_SDK_SRC ${AUTOTALKS_SDK_SRC}/cv2x-secton-5.19.1-rel)
set(AUTOTALKS_BUILD /home/eee-rpi/cv2x_testing/secton_cv2x)
set(AUTOTALKS_ARCH armv8-64)

# Object directories
set(AUTOTALKS_OBJ_DIR ${AUTOTALKS_SDK_SRC}/output.sec/${AUTOTALKS_ARCH}/obj)
set(AUTOTALKS_CV2X_OBJ_DIR ${AUTOTALKS_CV2X_SDK_SRC}/output.sec/${AUTOTALKS_ARCH}/obj)

# ASN.1 Auto-generated Sources
file(GLOB ASN1_SOURCES "${AUTOTALKS_CV2X_SDK_SRC}/output.sec/${AUTOTALKS_ARCH}/apps/auto-gen/*.c")

# Base SDK Object Files
set(AUTOTALKS_BASE_OBJECTS
    target_type/target_type_remote.o
    platform_type/platform_type_linux_u.o
    link_layer/linux_u/remote/link_layer_remote_spi.o
    link_layer/linux_u/ipc/link_layer_ipc.o
    link_layer/linux_u/tee/link_layer_tee_stub.o
    link_layer/link_layer_interface.o
    flash_reader/linux_u/remote/flash_reader_remote.o
    log_reader/log_reader.o
    secure_hdif_crypto_layer/crypto_layer_ref_sys.o
    secure_hdif_crypto_layer/crypto_layer_interface.o
    secure_hdif_crypto_layer/crypto_lib_wrapper.o
    secure_storage/secure_storage.o
    poti_lib/gnss/gnss.o
    poti_lib/nmea/nmea.o
    poti_lib/nmea/nmea_io.o
    poti_lib/nmea/nmea_io_check.o
    poti_lib/nmea/nmea_nav.o
    poti_lib/nmea/nmea_parse.o
    poti_lib/gnss/teseo/teseo.o
    poti_lib/gnss/teseo/teseo_commands.o
    poti_lib/coord/coord.o
    poti_lib/periodic_alarm/periodic_alarm.o
    poti_lib/api/poti_api.o
    poti_lib/ubx/serial_driver.o
    poti_lib/ubx/ubx_driver.o
    poti_lib/ubx/ubx_nav.o
)
list(TRANSFORM AUTOTALKS_BASE_OBJECTS PREPEND ${AUTOTALKS_OBJ_DIR}/)

# CV2X Plugin Object Files
set(AUTOTALKS_CV2X_OBJECTS
    ref_sys/ref_cv2x_sys.o
    ref_sys/ref_sys.o
    ref_sys/ref_sys_logger.o
    time_sync/time_sync.o
    time_sync/time_sync_poti.o
)
list(TRANSFORM AUTOTALKS_CV2X_OBJECTS PREPEND ${AUTOTALKS_CV2X_OBJ_DIR}/)

# Include Directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${AUTOTALKS_SDK_SRC}/include
    ${AUTOTALKS_SDK_SRC}/src/include
    ${AUTOTALKS_SDK_SRC}/src/core/include
    ${AUTOTALKS_SDK_SRC}/ref_src/include
    ${AUTOTALKS_SDK_SRC}/depend/device/include
    ${AUTOTALKS_CV2X_SDK_SRC}/include
    ${AUTOTALKS_CV2X_SDK_SRC}/ref_src/include
    ${AUTOTALKS_CV2X_SDK_SRC}/output.sec/${AUTOTALKS_ARCH}/apps/auto-gen
)

link_directories(${AUTOTALKS_BUILD}/lib)

# Build Executable
add_executable(cv2x_rpc_server
    ${CAPNP_SRCS}
    ${CAPNP_HDRS}
    autotalks_wrapper.cpp
    cv2x_rpc_server.cpp
    ${ASN1_SOURCES}
)

# Link Libraries
target_link_libraries(cv2x_rpc_server
    CapnProto::capnp-rpc
    CapnProto::capnp
    CapnProto::kj
    CapnProto::kj-async
    atlkcv2x
    atlkremote_linux_u
    ${AUTOTALKS_CV2X_OBJECTS}
    ${AUTOTALKS_BASE_OBJECTS}
    pthread
    rt
)

# Set RPATH
set_target_properties(cv2x_rpc_server PROPERTIES
    INSTALL_RPATH "${AUTOTALKS_BUILD}/lib"
    BUILD_WITH_INSTALL_RPATH TRUE
)

install(TARGETS cv2x_rpc_server DESTINATION bin)