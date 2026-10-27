file(GLOB_RECURSE RZA1_SOURCES CONFIGURE_DEPENDS *.S *.c *.cpp *.h)

add_library(RZA1 STATIC ${RZA1_SOURCES})
target_include_directories(RZA1 PRIVATE ${SHARED_INCLUDE} ${CMAKE_CURRENT_SOURCE_DIR}/../deluge)
target_link_libraries(RZA1 PRIVATE fatfs)
set_target_properties(RZA1 PROPERTIES CXX_STANDARD ${DELUGE_CXX_STANDARD} CXX_STANDARD_REQUIRED ON)

target_compile_definitions(RZA1 PRIVATE
    $<$<CONFIG:DEBUG>:ENABLE_TEXT_OUTPUT=1>
)

if(ENABLE_RTT)
  message(STATUS "RTT enabled for RZA1")

  target_link_libraries(RZA1 PRIVATE RTT)

  target_compile_definitions(RZA1 PRIVATE
      $<$<CONFIG:DEBUG>:HAVE_RTT=1>
  )
endif(ENABLE_RTT)
