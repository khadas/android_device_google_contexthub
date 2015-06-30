SELF_DIR := $(SELF_MKFILE:Makefile=)
SELF_FILES := $(wildcard $(SELF_DIR)*.c)
APP_NM := $(SELF_DIR)app
CLEANFILES := $(CLEANFILES) $(APP_NM).elf  $(APP_NM).bin
DELIVERABLES := $(DELIVERABLES) $(APP_NM).app
APP_ELF := $(APP_NM).elf
APP_BIN := $(APP_NM).bin
APP_APP := $(APP_NM).app


define APPRULE
$(APP_APP): $(APP_BIN)
	nanoapp_postprocess -v $(APP_ID) < $(APP_BIN) > $(APP_APP)

$(APP_BIN): $(APP_ELF)
	$(OBJCOPY) -j.relocs -j.flash -j.data -j.dynsym -O binary $(APP_ELF) $(APP_BIN)

$(APP_ELF): $(SELF_FILES)
	$(GCC) -o $(APP_ELF) $(FLAGS) $(APPFLAGS) $(SELF_FILES)
endef

$(eval $(APPRULE))
