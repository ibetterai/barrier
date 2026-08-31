# barrier -- mouse and keyboard sharing utility
# Copyright (C) 2012-2016 Symless Ltd.
# Copyright (C) 2009 Nick Bolton
#
# This package is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# found in the file LICENSE that should have accompanied this file.
#
# This package is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

file(GLOB_RECURSE headers "*.h")
file(GLOB_RECURSE sources "*.cpp")

# New tests are registered explicitly so a stale configure-time glob
# cache cannot silently drop them; the guard keeps the entry single.
if (NOT "barrier/DisplayGeometryTests.cpp" IN_LIST sources AND
    NOT "${CMAKE_CURRENT_SOURCE_DIR}/barrier/DisplayGeometryTests.cpp" IN_LIST sources)
    list(APPEND sources "${CMAKE_CURRENT_SOURCE_DIR}/barrier/DisplayGeometryTests.cpp")
endif()
if (NOT "barrier/DisplayNamesTests.cpp" IN_LIST sources AND
    NOT "${CMAKE_CURRENT_SOURCE_DIR}/barrier/DisplayNamesTests.cpp" IN_LIST sources)
    list(APPEND sources "${CMAKE_CURRENT_SOURCE_DIR}/barrier/DisplayNamesTests.cpp")
endif()
if (NOT "barrier/ClientProxyDisplayNamesTests.cpp" IN_LIST sources AND
    NOT "${CMAKE_CURRENT_SOURCE_DIR}/barrier/ClientProxyDisplayNamesTests.cpp" IN_LIST sources)
    list(APPEND sources "${CMAKE_CURRENT_SOURCE_DIR}/barrier/ClientProxyDisplayNamesTests.cpp")
endif()

if (NOT "platform/OSXScreenTests.cpp" IN_LIST sources AND
    NOT "${CMAKE_CURRENT_SOURCE_DIR}/platform/OSXScreenTests.cpp" IN_LIST sources)
    list(APPEND sources "${CMAKE_CURRENT_SOURCE_DIR}/platform/OSXScreenTests.cpp")
endif()
if (NOT "base/Sha256Tests.cpp" IN_LIST sources AND
    NOT "${CMAKE_CURRENT_SOURCE_DIR}/base/Sha256Tests.cpp" IN_LIST sources)
    list(APPEND sources "${CMAKE_CURRENT_SOURCE_DIR}/base/Sha256Tests.cpp")
endif()
if (NOT "base/EventQueueTests.cpp" IN_LIST sources AND
    NOT "${CMAKE_CURRENT_SOURCE_DIR}/base/EventQueueTests.cpp" IN_LIST sources)
    list(APPEND sources "${CMAKE_CURRENT_SOURCE_DIR}/base/EventQueueTests.cpp")
endif()
if (NOT "barrier/DisplayTopologyTests.cpp" IN_LIST sources AND
    NOT "${CMAKE_CURRENT_SOURCE_DIR}/barrier/DisplayTopologyTests.cpp" IN_LIST sources)
    list(APPEND sources "${CMAKE_CURRENT_SOURCE_DIR}/barrier/DisplayTopologyTests.cpp")
endif()
if (NOT "barrier/TopologyProfileConfigTests.cpp" IN_LIST sources AND
    NOT "${CMAKE_CURRENT_SOURCE_DIR}/barrier/TopologyProfileConfigTests.cpp" IN_LIST sources)
    list(APPEND sources "${CMAKE_CURRENT_SOURCE_DIR}/barrier/TopologyProfileConfigTests.cpp")
endif()
if (NOT "server/DisplayTopologyStateMachineTests.cpp" IN_LIST sources AND
    NOT "${CMAKE_CURRENT_SOURCE_DIR}/server/DisplayTopologyStateMachineTests.cpp" IN_LIST sources)
    list(APPEND sources "${CMAKE_CURRENT_SOURCE_DIR}/server/DisplayTopologyStateMachineTests.cpp")
endif()
if (NOT "server/ServerTests.cpp" IN_LIST sources AND
    NOT "${CMAKE_CURRENT_SOURCE_DIR}/server/ServerTests.cpp" IN_LIST sources)
    list(APPEND sources "${CMAKE_CURRENT_SOURCE_DIR}/server/ServerTests.cpp")
endif()
if (NOT "server/ClientWakeRequestTests.cpp" IN_LIST sources AND
    NOT "${CMAKE_CURRENT_SOURCE_DIR}/server/ClientWakeRequestTests.cpp" IN_LIST sources)
    list(APPEND sources "${CMAKE_CURRENT_SOURCE_DIR}/server/ClientWakeRequestTests.cpp")
endif()
if (NOT "net/NetworkAddressSelectionTests.cpp" IN_LIST sources AND
    NOT "${CMAKE_CURRENT_SOURCE_DIR}/net/NetworkAddressSelectionTests.cpp" IN_LIST sources)
    list(APPEND sources "${CMAKE_CURRENT_SOURCE_DIR}/net/NetworkAddressSelectionTests.cpp")
endif()


file(GLOB_RECURSE remove_platform "platform/*")
list(REMOVE_ITEM headers ${remove_platform})
list(REMOVE_ITEM sources ${remove_platform})

file(GLOB_RECURSE global_headers "../../test/global/*.h")
file(GLOB_RECURSE global_sources "../../test/global/*.cpp")

list(APPEND headers ${global_headers})
list(APPEND sources ${global_sources})

file(GLOB_RECURSE mock_headers "../../test/mock/*.h")
file(GLOB_RECURSE mock_sources "../../test/mock/*.cpp")

list(APPEND headers ${mock_headers})
list(APPEND sources ${mock_sources})

# platform
if (WIN32)
    file(GLOB platform_sources "platform/MSWindows*.cpp")
    file(GLOB platform_headers "platform/MSWindows*.h")
elseif (APPLE)
    file(GLOB platform_sources "platform/OSX*.cpp")
    file(GLOB platform_headers "platform/OSX*.h")
elseif (UNIX)
    file(GLOB platform_sources "platform/XWindows*.cpp")
    file(GLOB platform_headers "platform/XWindows*.h")
endif()

# platform test sources must compile under every generator; the
# BARRIER_ADD_HEADERS path alone drops them for Unix Makefiles, which
# would silently skip all OSX/MSWindows/XWindows unit tests
list(APPEND sources ${platform_sources})
list(APPEND headers ${platform_headers})

include_directories(
    ../../
    ../../../ext
)

if (UNIX)
    include_directories(
        ../../..
    )
endif()

if (BARRIER_ADD_HEADERS)
    list(APPEND sources ${headers})
endif()

add_executable(unittests ${sources})
target_link_libraries(unittests
    arch base client server common io net platform server synlib mt ipc ${GTEST_LIBRARIES} ${GMOCK_LIBRARIES} ${libs} ${OPENSSL_LIBS})
