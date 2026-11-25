# kode-sdk at the release the apps pin, for the suites that run on its own QP/C
# (tests/host_ao and tests/host_sys): its headers, its QP/C and kqp_isr.c. Not
# through the component manager: that fetches every layer kodediy/kode depends
# on and configures them all, and some of what they need (esp_eth, for the C5's
# link) does not exist for `linux`. One shallow clone instead, kept in
# tests/host_ao (.gitignore); delete it to fetch again.
#
# Sets KODE_SDK_VERSION and KODE_SDK_DIR.
set(KODE_SDK_VERSION "v0.1.5")
set(KODE_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/host_ao/kode-sdk-${KODE_SDK_VERSION}")
if(NOT EXISTS "${KODE_SDK_DIR}/components/kode_sys/include/kode_sys.h")
    file(REMOVE_RECURSE "${KODE_SDK_DIR}")
    execute_process(
        COMMAND git clone --quiet --depth 1 --branch "${KODE_SDK_VERSION}"
                "https://github.com/kodediy/kode-sdk.git" "${KODE_SDK_DIR}"
        RESULT_VARIABLE fetched)
    if(NOT fetched EQUAL 0)
        message(FATAL_ERROR "kode-sdk ${KODE_SDK_VERSION} could not be fetched")
    endif()
endif()
