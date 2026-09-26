# dx12um_virtio.dll: the D3D12 user-mode driver (OpenAdapter12), vkd3d-proton
# linked in statically. Built by win3d/build_d3d12.sh, which also builds the
# vkd3d-proton static libraries this links against (VKD3D_BUILD).
PREFIX?=aarch64-w64-mingw32
BUILD_DIR?=build
VKD3D_SRC?=../vkd3d-proton
VKD3D_BUILD?=$(VKD3D_SRC)/build.umd-$(if $(findstring arm64ec,$(PREFIX)),arm64ec,arm64)
D3D11_DIR?=../virtio-d3d11
LTO?=

CC=$(PREFIX)-clang
CXX=$(PREFIX)-clang++
OBJCOPY=$(PREFIX)-objcopy
STRIP=$(PREFIX)-strip

INCLUDES=include/winddk $(D3D11_DIR)/include/winddk $(D3D11_DIR)/include \
         $(VKD3D_SRC)/include $(VKD3D_SRC)/khronos/Vulkan-Headers/include
CFLAGS=-std=gnu23 -O2 -g -Wall -Wno-unknown-pragmas -Wno-missing-braces -ffunction-sections -fdata-sections \
       -MMD -MP $(LTO) -ffile-prefix-map=$(CURDIR)=./
# -iquote: only "vk_bootstrap.h" comes from dx11um; its dxgi.h must not shadow <dxgi.h>.
CFLAGS+=$(addprefix -I,$(INCLUDES)) -iquote $(D3D11_DIR)
LDFLAGS=-g -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic -Wl,--gc-sections
LIBS=version gdi32 user32 advapi32 ole32 dxguid pathcch

# vkd3d-proton without libs/d3d12 and libs/d3d12core: those are the DLL entry
# points and pull in dxgi.dll; the runtime talks to us through the DDI instead.
VKD3D_LIBS=$(addprefix $(VKD3D_BUILD)/,libs/vkd3d/libvkd3d-proton.a libs/vkd3d-shader/libvkd3d-shader.a \
           libs/vkd3d-common/libvkd3d_common.a subprojects/dxil-spirv/libdxil-spirv.a \
           subprojects/dxil-spirv/libdxbc_spv_module.a subprojects/dxil-spirv/subprojects/dxbc-spirv/libdxbc_spv.a)

UMD_NAME := dx12um_virtio
OBJS=$(addprefix $(BUILD_DIR)/,d3d12_adapter.o d3d12_caps.o d3d12_device.o d3d12_objects.o d3d12_resource.o d3d12_present.o \
     d3d12_descriptor.o d3d12_pipeline.o d3d12_cmdlist.o d3d12_queue.o ddi_stubs.o d3d12_vkd3d_glue.o \
     d3d12_kmt_trace.o vk_bootstrap.o)

.DEFAULT_GOAL := $(BUILD_DIR)/dist/$(UMD_NAME).dll

-include $(wildcard $(BUILD_DIR)/*.d)

# Regenerate after an SDK header change; the output is checked in.
.PHONY: stubs
stubs:
	python3 gen_ddi_stubs.py --cc $(CC) $(addprefix -I,$(INCLUDES)) -o ddi_stubs.c

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)

$(BUILD_DIR)/$(UMD_NAME).dll: $(OBJS) $(UMD_NAME).def $(VKD3D_LIBS) | $(BUILD_DIR)
	$(CXX) $(CFLAGS) -shared -o $@ $(OBJS) $(VKD3D_LIBS) -Wl,$(UMD_NAME).def $(LDFLAGS) $(addprefix -l,$(LIBS))

$(BUILD_DIR)/dist/$(UMD_NAME).dll: $(BUILD_DIR)/$(UMD_NAME).dll | $(BUILD_DIR)/dist
	$(OBJCOPY) --only-keep-debug $< $(BUILD_DIR)/dist/$(UMD_NAME).debug
	cp $< $@
	$(STRIP) --strip-debug --strip-unneeded $@
	$(OBJCOPY) --add-gnu-debuglink=$(BUILD_DIR)/dist/$(UMD_NAME).debug $@

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -c -o $@

$(BUILD_DIR)/vk_bootstrap.o: $(D3D11_DIR)/vk_bootstrap.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -c -o $@

$(BUILD_DIR) $(BUILD_DIR)/dist:
	mkdir -p $@
