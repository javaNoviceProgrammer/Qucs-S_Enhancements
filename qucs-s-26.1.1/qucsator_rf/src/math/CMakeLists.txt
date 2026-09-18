include_directories( ${CMAKE_CURRENT_SOURCE_DIR})

set(MATH_SRC # cbesselj.cpp
    complex.cpp fspecial.cpp matrix.cpp )

set(HEADERS complex.h matrix.h precision.h real.h)

add_library(coreMath OBJECT ${MATH_SRC})

if(WITH_ADMS)
install(FILES ${HEADERS} DESTINATION include/qucs-core)
endif()
