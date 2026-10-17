# Nintendo 3DS build (README.md §6.28): mgs_mobile.3dsx. Needs devkitPro with 3ds-dev (devkitARM, libctru,
# citro3d). From the devkitPro MSYS2 shell: make -C mgs_native/3ds   (build.bat in this folder does that)
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "DEVKITARM isn't set: run from the devkitPro shell (source /etc/profile.d/devkit-env.sh)")
endif

TOPDIR := .
include $(DEVKITARM)/3ds_rules

TARGET      := mgs_mobile
NATIVE      := ..
TP          := $(NATIVE)/third_party
BUILD       := build
APP_TITLE   := Metal Gear Solid Mobile
APP_DESCRIPTION := N-Gage 2.0 game, native port
APP_AUTHOR  := MGS Mobile port
ICON        := $(TOPDIR)/icon.png

# The shared game code (the same files as the desktop / Android builds, minus SDL's: main, audio, gamepad,
# platform) plus this folder's 3DS layer.
SHARED := loader vfs runtime gles heap settings luachunk story gametools photomode screenshot pngio textures \
          custommodel installer autotest keys cpu_native cpu_dynarmic guest_interp touchcontrols movement \
          assetviewer modelsandbox overlay imgui_impl_gles1
PORT   := main_3ds platform_3ds gamepad_3ds audio_3ds gl3ds
IMGUI  := imgui imgui_draw imgui_tables imgui_widgets
SHADERS := gl3ds

OFILES := $(addprefix $(BUILD)/,$(addsuffix .o,$(SHARED) $(PORT) $(IMGUI)) miniz.o LzmaDec.o native_arm.o) \
          $(addprefix $(BUILD)/,$(addsuffix .shbin.o,$(SHADERS)))

ARCH     := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
DEFINES  := -D__3DS__ -DMGS_GLES -DMGS_3DS
INCLUDE  := -I$(NATIVE) -I$(TOPDIR) -I$(BUILD) -I$(TP) -I$(TP)/imgui -I$(TP)/miniz -I$(TP)/lzma -I$(TP)/gles1 \
            -I$(TP)/SDL3/include -I$(CTRULIB)/include -I$(PORTLIBS)/include
CFLAGS   := -O3 -flto=auto -g -Wall -Wno-unused-parameter -mword-relocations -ffunction-sections $(ARCH) $(DEFINES) $(INCLUDE)
CXXFLAGS := $(CFLAGS) -std=gnu++17 -Wno-psabi
ASFLAGS  := -g $(ARCH) $(DEFINES)
LDFLAGS  := -specs=3dsx.specs -O3 -flto=auto -g $(ARCH) -Wl,-Map,$(BUILD)/$(TARGET).map -Wl,--gc-sections
LIBS     := -L$(CTRULIB)/lib -L$(PORTLIBS)/lib -lcitro3d -lctru -lm

.PHONY: all clean
all: $(TOPDIR)/$(TARGET).3dsx

clean:
	@rm -rf $(BUILD) $(TOPDIR)/$(TARGET).3dsx $(TOPDIR)/$(TARGET).smdh $(TOPDIR)/$(TARGET).elf

$(TOPDIR)/$(TARGET).3dsx: $(TOPDIR)/$(TARGET).elf $(TOPDIR)/$(TARGET).smdh
	@3dsxtool $< $@ --smdh=$(TOPDIR)/$(TARGET).smdh
	@echo built ... $(notdir $@)

$(TOPDIR)/$(TARGET).smdh: $(ICON)
	@smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" $(ICON) $@

$(TOPDIR)/$(TARGET).elf: $(OFILES)
	@echo linking $(notdir $@)
	@$(CXX) $(LDFLAGS) $(OFILES) $(LIBS) -o $@

$(BUILD):
	@mkdir -p $@

# gl3ds.cpp includes the assembled shader's header
$(BUILD)/gl3ds.o: $(BUILD)/gl3ds_shbin.h

$(BUILD)/%.o: $(NATIVE)/%.cpp | $(BUILD)
	@echo $(notdir $<)
	@$(CXX) -MMD -MP $(CXXFLAGS) -c $< -o $@
$(BUILD)/%.o: $(TOPDIR)/%.cpp | $(BUILD)
	@echo $(notdir $<)
	@$(CXX) -MMD -MP $(CXXFLAGS) -c $< -o $@
$(BUILD)/%.o: $(TP)/imgui/%.cpp | $(BUILD)
	@echo $(notdir $<)
	@$(CXX) -MMD -MP $(CXXFLAGS) -c $< -o $@
$(BUILD)/miniz.o: $(TP)/miniz/miniz.c | $(BUILD)
	@echo $(notdir $<)
	@$(CC) -MMD -MP $(CFLAGS) -c $< -o $@
$(BUILD)/LzmaDec.o: $(TP)/lzma/LzmaDec.c | $(BUILD)
	@echo $(notdir $<)
	@$(CC) -MMD -MP $(CFLAGS) -c $< -o $@
$(BUILD)/native_arm.o: $(NATIVE)/native_arm.S | $(BUILD)
	@echo $(notdir $<)
	@$(CC) -MMD -MP -x assembler-with-cpp $(ASFLAGS) -c $< -o $@

# Vertex shader: picasso -> .shbin, embedded with bin2s (gl3ds_shbin, gl3ds_shbin_size)
$(BUILD)/gl3ds.shbin: $(TOPDIR)/gl3ds.v.pica | $(BUILD)
	@echo $(notdir $<)
	@picasso -o $@ $<
$(BUILD)/gl3ds.shbin.o $(BUILD)/gl3ds_shbin.h: $(BUILD)/gl3ds.shbin
	@bin2s -a 4 -H $(BUILD)/gl3ds_shbin.h $(BUILD)/gl3ds.shbin | $(AS) -o $(BUILD)/gl3ds.shbin.o

-include $(wildcard $(BUILD)/*.d)
