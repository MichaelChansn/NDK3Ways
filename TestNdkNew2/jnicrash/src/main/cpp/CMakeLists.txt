
cmake_minimum_required(VERSION 3.4.1)

project(jnicrash)

aux_source_directory(. DIR_SRCS)
aux_source_directory(./corkscrew CORKSCREW)
aux_source_directory(./corkscrew/arch-arm CORKSCREW_ARCH)
aux_source_directory(./cutils CUTILS)
aux_source_directory(./debuggerd DEBUGGERD)
aux_source_directory(./debuggerd/arm DEBUGGERD_ARM)
aux_source_directory(./handler HANDLER)

list(APPEND DIR_SRCS ${CORKSCREW})
list(APPEND DIR_SRCS ${CORKSCREW_ARCH})
list(APPEND DIR_SRCS ${CUTILS})
list(APPEND DIR_SRCS ${DEBUGGERD})
list(APPEND DIR_SRCS ${DEBUGGERD_ARM})
list(APPEND DIR_SRCS ${HANDLER})

add_library(jnicrash SHARED ${DIR_SRCS} )

include_directories(${DIR_SRCS})

target_link_libraries(jnicrash log z m)