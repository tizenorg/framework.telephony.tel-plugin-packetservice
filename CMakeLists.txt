CMAKE_MINIMUM_REQUIRED(VERSION 2.6)
PROJECT(ps-plugin C)

#INCLUDE(FindPkgConfig)

SET(PREFIX ${CMAKE_INSTALL_PREFIX})
SET(EXEC_PREFIX "\${prefix}")
SET(LIBDIR "\${prefix}/lib")
SET(INCLUDEDIR "\${prefix}/include")
SET(DATAROOTDIR "\${prefix}/share")
if(NOT DEFINED SYSCONFDIR)
	SET(SYSCONFDIR "/etc")
endif()

# Set required packages
INCLUDE(FindPkgConfig)
pkg_check_modules(pkgs REQUIRED glib-2.0 gio-2.0 gio-unix-2.0 tcore iniparser libxml-2.0)

FOREACH(flag ${pkgs_CFLAGS})
	SET(EXTRA_CFLAGS "${EXTRA_CFLAGS} ${flag}")
ENDFOREACH(flag)

INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/include/ ${CMAKE_SOURCE_DIR}/cmake_tmp )

SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${EXTRA_CFLAGS} -Werror -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wdeclaration-after-statement -Wmissing-declarations -Wcast-align -Wall -Wno-array-bounds -Wno-empty-body -Wno-ignored-qualifiers -Wshadow -Wswitch-default -Wno-unused-but-set-parameter -Wno-unused-but-set-variable")

### Purge unused code ###
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fdata-sections -ffunction-sections -Wl,--gc-sections")

# model definitions #
IF (CONNECT_DEFAULT_CONNECTION_WITHOUT_TIMER)
  ADD_DEFINITIONS("-DCONNECT_DEFAULT_CONNECTION_WITHOUT_TIMER")
ENDIF (CONNECT_DEFAULT_CONNECTION_WITHOUT_TIMER)

ADD_DEFINITIONS("-DFEATURE_TLOG_DEBUG")
ADD_DEFINITIONS("-DTCORE_LOG_TAG=\"PS\"")
ADD_DEFINITIONS("-DPLUGIN_VERSION=${VERSION}")
ADD_DEFINITIONS("-DEXPORT_API=__attribute__((visibility(\"default\")))")

MESSAGE(${CMAKE_C_FLAGS})
MESSAGE(${pkgs_LDFLAGS})

SET(SRCS
	src/desc.c
	src/master.c
	src/modem.c
	src/service.c
	src/context.c
	src/tcore-interface.c
	src/ps_log.c
	src/util.c
	${CMAKE_BINARY_DIR}/generated-code.c
)

ADD_CUSTOM_COMMAND(
	OUTPUT ${CMAKE_BINARY_DIR}/generated-code.c
	COMMAND gdbus-codegen --interface-prefix com.tcore.ps. --generate-c-code generated-code --c-namespace PacketService --c-generate-object-manager --generate-docbook generated-docs ${CMAKE_SOURCE_DIR}/introspection/master.xml ${CMAKE_SOURCE_DIR}/introspection/modem.xml ${CMAKE_SOURCE_DIR}/introspection/service.xml ${CMAKE_SOURCE_DIR}/introspection/context.xml
	COMMENT "Generating GDBus .c/.h")

# library build
ADD_LIBRARY(ps-plugin SHARED ${SRCS})
TARGET_LINK_LIBRARIES(ps-plugin ${pkgs_LDFLAGS})
SET_TARGET_PROPERTIES(ps-plugin PROPERTIES PREFIX "" OUTPUT_NAME ps-plugin)

# install
INSTALL(FILES ${CMAKE_SOURCE_DIR}/resources/dnet_db.sql DESTINATION ${DATAROOTDIR}/ps-plugin)
INSTALL(FILES ${CMAKE_SOURCE_DIR}/resources/dnet_db_init.sql DESTINATION ${DATAROOTDIR}/ps-plugin)
INSTALL(FILES ${CMAKE_SOURCE_DIR}/resources/apns-conf.xml DESTINATION ${DATAROOTDIR}/ps-plugin)
INSTALL(FILES ${CMAKE_SOURCE_DIR}/resources/520.tel-plugin-packetservice.patch.sh DESTINATION ${SYSCONFDIR}/opt/upgrade)
INSTALL(TARGETS ps-plugin LIBRARY DESTINATION lib/telephony/plugins)
INSTALL(FILES ${CMAKE_SOURCE_DIR}/LICENSE DESTINATION /usr/share/license RENAME tel-plugin-packetservice)
INSTALL(FILES ${CMAKE_SOURCE_DIR}/src/dump_packetservice.sh DESTINATION /opt/etc/dump.d/module.d)
