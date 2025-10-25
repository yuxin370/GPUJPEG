cmake_minimum_required(VERSION 3.8.0 FATAL_ERROR)
project(gpujpeg-examples LANGUAGES C)

# Try to use gpujpeg target if already defined (i.e., when added via add_subdirectory)
if(TARGET gpujpeg)
    # Being built as part of the main GPUJPEG project
    set(GPUJPEG_LIBRARIES gpujpeg)
else()
    # Being built standalone: need to find_package
    find_package(GPUJPEG REQUIRED)
    set(GPUJPEG_LIBRARIES GPUJPEG::gpujpeg)  # or just gpujpeg if imported as such
endif()

add_executable(encode encode.c)
target_link_libraries(encode ${GPUJPEG_LIBRARIES})

add_executable(encode_minimal encode_minimal.c)
target_link_libraries(encode_minimal ${GPUJPEG_LIBRARIES})

add_executable(encode_raw encode_raw.c)
target_link_libraries(encode_raw ${GPUJPEG_LIBRARIES})

add_executable(decode_minimal decode_minimal.c)
target_link_libraries(decode_minimal ${GPUJPEG_LIBRARIES})

add_executable(decode_to_pnm decode_to_pnm.c)
target_link_libraries(decode_to_pnm ${GPUJPEG_LIBRARIES})

add_executable(decode_to_raw_rgb decode_to_raw_rgb.c)
target_link_libraries(decode_to_raw_rgb ${GPUJPEG_LIBRARIES})

add_executable(idct_from_coeffs idct_from_coeffs.c)
target_link_libraries(idct_from_coeffs ${GPUJPEG_LIBRARIES})

include(CheckLanguage)
check_language(CUDA)
if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
    add_executable(decode_to_cuda_pnm decode_to_cuda_pnm.c)
    target_link_libraries(decode_to_cuda_pnm ${GPUJPEG_LIBRARIES})
    target_link_libraries(decode_to_cuda_pnm CUDA::cudart)
endif()