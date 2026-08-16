project(3dfx_amiga C)

set(FX3D_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../3dfx_dos")

add_library(3dfx_amiga STATIC
    ${FX3D_SOURCE_DIR}/match.c
    ${FX3D_SOURCE_DIR}/devclut.c
    ${FX3D_SOURCE_DIR}/device.c
    ${FX3D_SOURCE_DIR}/plib.c
    ${FX3D_SOURCE_DIR}/outfcty.c
    ${FX3D_SOURCE_DIR}/devpixmp.c
    ${FX3D_SOURCE_DIR}/devpmsys.c
    ${FX3D_SOURCE_DIR}/sbuffer.c
    ${FX3D_SOURCE_DIR}/rendfunc.c
    ${FX3D_SOURCE_DIR}/hstdlib.c
    ${FX3D_SOURCE_DIR}/pstate.c
    ${FX3D_SOURCE_DIR}/scale.c
    ${FX3D_SOURCE_DIR}/texture.c
    glide_shim.c
)

target_compile_definitions(3dfx_amiga PRIVATE
    BrDrv1Begin=BrDrv13DFXAmigaBegin
    # NOTE: deliberately NOT defining AMIGA here.  `if(AMIGA)` in
    # drivers/CMakeLists.txt is only a CMake variable, so the #ifdef AMIGA
    # blocks in the shared 3dfx_dos sources have never been compiled in, and
    # the renderer was tuned with them absent.  Enabling all of them at once
    # regressed the HUD and pedestrian colours.  The one block that is wanted
    # -- Clear3DfxStateCache() after the direct 2D blit -- is enabled on its
    # own via FXA_CLEAR_STATE_AFTER_BLIT below.
    FXA_CLEAR_STATE_AFTER_BLIT=1
)

target_include_directories(3dfx_amiga PRIVATE
    ${FX3D_SOURCE_DIR}
    ${FX3D_SOURCE_DIR}/glide/dos
    ${CMAKE_CURRENT_SOURCE_DIR}/../softrend/ddi
)

target_link_libraries(3dfx_amiga PRIVATE
    BRender::DDI
    ${AMIGA_SUPPORT_PATH}/lib/libmgl-new.a
)

set_target_properties(3dfx_amiga PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED TRUE
    FOLDER "BRender/Drivers"
)
