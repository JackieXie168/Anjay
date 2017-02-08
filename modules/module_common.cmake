make_absolute_sources(ABSOLUTE_MODULE_SOURCES
                      ${SOURCES}
                      ${PRIVATE_HEADERS}
                      ${MODULES_HEADERS}
                      ${PUBLIC_HEADERS})

make_absolute_sources(ABSOLUTE_MODULE_TEST_SOURCES
                      ${TEST_SOURCES})

list(APPEND ABSOLUTE_SOURCES ${ABSOLUTE_MODULE_SOURCES})
set(ABSOLUTE_SOURCES "${ABSOLUTE_SOURCES}" PARENT_SCOPE)

list(APPEND ABSOLUTE_TEST_SOURCES ${ABSOLUTE_MODULE_TEST_SOURCES})
set(ABSOLUTE_TEST_SOURCES "${ABSOLUTE_TEST_SOURCES}" PARENT_SCOPE)