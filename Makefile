
MAKEDIR = '$(SHELL)' -c "mkdir -p \"$(1)\""
RM = '$(SHELL)' -c "rm -rf \"$(1)\""

OBJDIR := BUILD
SRCDIR := $(CURDIR)

.PHONY: clean

.SUFFIXES:
.SUFFIXES: .cpp .o

vpath %.cpp .
vpath %.c .
vpath %.h .
vpath %.s .

PROJECT := gm0
BPROJECT := BUILD/gm0

#SOURCES += $(shell find gdl -type f -iname '*.c' -o -iname '*.s'  -o -iname '*.cpp')
SOURCES += $(shell find fgb -type f -iname '*.c' -o -iname '*.s'  -o -iname '*.cpp')
SOURCES += $(shell find pokitto -maxdepth 1 -type f -iname '*.c' -o -iname '*.s' -o -iname '*.cpp')

SOURCES += $(shell find pokitto/MCP23S17 -maxdepth 1 -type f -iname '*.c' -o -iname '*.s' -o -iname '*.cpp')


SOURCES += $(shell find . -maxdepth 1 -type f -iname 'main.cpp')

SOURCES += $(shell find mbed-pokitto -type f -iname '*.c' -o -iname '*.s'  -o -iname '*.cpp')

#SOURCE += "main.cpp"

OBJECTS = $(foreach x, $(basename $(SOURCES)), BUILD/$(x).o)

#OBJECTS += BUILD/main.o

INCLUDE_PATHS += -I. -I./pokitto -I./gdl

FOLDERS=$(shell find mbed-pokitto -type d)
INCLUDE_PATHS += $(foreach x, $(basename $(FOLDERS)), -I./$(x))

LIBRARY_PATHS :=
LIBRARIES :=
LINKER_SCRIPT = ./pokitto/LPC11U68.ld

#COMMONS = '-c' '-Wall' '-Wextra' '-Wignored-qualifiers' '-Wno-unused-variable' '-Wno-array-bounds' '-Wno-unused-function' '-Wno-unused-parameter' '-Wno-missing-field-initializers' '-fmessage-length=0' '-fno-exceptions' '-fno-builtin' '-ffunction-sections' '-fdata-sections' '-funsigned-char' '-fno-delete-null-pointer-checks' '-fomit-frame-pointer' '-O3' '-g0' '-s' '-DNDEBUG' '-DMBED_RTOS_SINGLE_THREAD' '-mtune=cortex-m0plus' '-mcpu=cortex-m0plus' '-mthumb'
COMMONS =  '-c' '-Wall' '-Wextra' '-Wignored-qualifiers' '-Wno-unused-variable' '-Wno-array-bounds' '-Wno-unused-function' '-Wno-unused-parameter' '-O3' '-g0' '-s' '-DNDEBUG' '-DMBED_RTOS_SINGLE_THREAD' '-mtune=cortex-m0plus' '-mcpu=cortex-m0plus' '-mthumb'

AS      = 'arm-none-eabi-gcc' '-x' 'assembler-with-cpp'
CC      = 'arm-none-eabi-gcc' '-Wno-old-style-declaration' ''-Wno-discarded-qualifiers''
CPP     = 'arm-none-eabi-g++' '-fno-rtti' '-Wvla'
LD      = 'arm-none-eabi-gcc' '-specs=nosys.specs'

AS += $(COMMONS)
CC += $(COMMONS)
CPP += $(COMMONS)

ELF2BIN = 'arm-none-eabi-objcopy'
PREPROC = 'arm-none-eabi-cpp' '-E' '-P' '-Wl,--gc-sections' '-Wl,--wrap,main' '-Wl,--wrap,_malloc_r' '-Wl,--wrap,_free_r' '-Wl,--wrap,_realloc_r' '-Wl,--wrap,_memalign_r' '-Wl,--wrap,_calloc_r' '-Wl,--wrap,exit' '-Wl,--wrap,atexit' '-Wl,-n' '--specs=nano.specs' '--specs=nosys.specs' '-mcpu=cortex-m0plus' '-mthumb'

CXX_FLAGS = -std=c++98 -fno-rtti -Wno-pointer-arith -c -fno-exceptions -ffunction-sections -fdata-sections -funsigned-char -MMD -fno-delete-null-pointer-checks -fomit-frame-pointer -mcpu=cortex-m0plus -mthumb -Wno-psabi -Wno-conversion-null -Wno-narrowing -Wno-write-strings -DTARGET_LPC11U68 -D__MBED__=1 -DDEVICE_I2CSLAVE=1 -DTARGET_LIKE_MBED -DTARGET_NXP -D__MBED_CMSIS_RTOS_CM -DDEVICE_RTC=1 -DTOOLCHAIN_object -D__CMSIS_RTOS -DTOOLCHAIN_GCC -DTARGET_CORTEX_M -DTARGET_M0P -DTARGET_UVISOR_UNSUPPORTED -DDEVICE_SERIAL=1 -DDEVICE_INTERRUPTIN=1 -DTARGET_LPCTarget -DTARGET_CORTEX -DDEVICE_I2C=1 -D__CORTEX_M0PLUS -DTARGET_FF_ARDUINO -DTARGET_RELEASE -DMBED_BUILD_TIMESTAMP=1526394586.66 -DARM_MATH_CM0PLUS -DTARGET_LPC11U6X -DDEVICE_SLEEP=1 -DTOOLCHAIN_GCC_ARM -DDEVICE_SPI=1 -DDEVICE_ANALOGIN=1 -DDEVICE_PWMOUT=1 -DTARGET_LIKE_CORTEX_M0

C_FLAGS = -std=gnu89 -Wno-pointer-arith -c -fno-exceptions -ffunction-sections -fdata-sections -funsigned-char -MMD -fno-delete-null-pointer-checks -fomit-frame-pointer -mcpu=cortex-m0plus -mthumb -Wno-psabi -Wwrite-strings -DTARGET_LPC11U68 -D__MBED__=1 -DDEVICE_I2CSLAVE=1 -DTARGET_LIKE_MBED -DTARGET_NXP -D__MBED_CMSIS_RTOS_CM -DDEVICE_RTC=1 -DTOOLCHAIN_object -D__CMSIS_RTOS -DTOOLCHAIN_GCC -DTARGET_CORTEX_M -DTARGET_M0P -DTARGET_UVISOR_UNSUPPORTED -DDEVICE_SERIAL=1 -DDEVICE_INTERRUPTIN=1 -DTARGET_LPCTarget -DTARGET_CORTEX -DDEVICE_I2C=1 -D__CORTEX_M0PLUS -DTARGET_FF_ARDUINO -DTARGET_RELEASE -DMBED_BUILD_TIMESTAMP=1526394586.66 -DARM_MATH_CM0PLUS -DTARGET_LPC11U6X -DDEVICE_SLEEP=1 -DTOOLCHAIN_GCC_ARM -DDEVICE_SPI=1 -DDEVICE_ANALOGIN=1 -DDEVICE_PWMOUT=1 -DTARGET_LIKE_CORTEX_M0

#ASM_FLAGS += -x
#ASM_FLAGS += assembler-with-cpp
#ASM_FLAGS += -D__CMSIS_RTOS
#ASM_FLAGS += -D__MBED_CMSIS_RTOS_CM
#ASM_FLAGS += -D__CORTEX_M0PLUS
#ASM_FLAGS += -DARM_MATH_CM0PLUS
#ASM_FLAGS += -I./.
#ASM_FLAGS += $(foreach x, $(basename $(FOLDERS)), -I./$(x))

#overclock
C_FLAGS += -D_OSCT=2
CXX_FLAGS += -D_OSCT=2
ASM_FLAGS += -D_OSCT=2

#included rom
ifeq ($(rom),)
rom := 2048
endif

C_FLAGS += -DROM=$(rom)
CXX_FLAGS += -DROM=$(rom)

LD_FLAGS = -Wl,--print-memory-usage -Wl,--gc-sections -Wl,-n -mcpu=cortex-m0plus -mthumb -Wl,--undefined=g_pfnVectors -Wl,--undefined=boot -Wl,--start-group -lstdc++ -lsupc++ -lm -lc -lgcc -lnosys -Wl,--end-group

.PHONY: all lst size

all: $(BPROJECT).bin $(BPROJECT).hex size
	+@$(call MAKEDIR,$(OBJDIR))

BUILD/%.o : %.s
	+@$(call MAKEDIR,$(dir $@))
	+@echo "Assemble: $(notdir $<)"
	@$(AS) -c $(ASM_FLAGS) -o $@ $<

BUILD/%.o : %.c
	+@$(call MAKEDIR,$(dir $@))
	+@echo "Compile: $(notdir $<)"
	@$(CC) $(C_FLAGS) $(INCLUDE_PATHS) -o $@ $<

BUILD/%.o : %.cpp
	+@$(call MAKEDIR,$(dir $@))
	+@echo "Compile: $(notdir $<)"
	@$(CPP) $(CXX_FLAGS) $(INCLUDE_PATHS) -o $@ $<


$(BPROJECT).link_script.ld: $(LINKER_SCRIPT)
	@$(PREPROC) $< -o $@

$(BPROJECT).elf: $(OBJECTS) $(SYS_OBJECTS) $(BPROJECT).link_script.ld
	+@echo "link: $(notdir $@)"
	@$(LD) $(LD_FLAGS) -T $(filter-out %.o, $^) $(LIBRARY_PATHS) --output $@ $(filter %.o, $^) $(LIBRARIES) $(LD_SYS_LIBS)


$(BPROJECT).bin: $(BPROJECT).elf
	$(ELF2BIN) -O binary $< $@
	+@echo "===== bin file ready to flash: $(OBJDIR)/$@ ====="

$(BPROJECT).hex: $(BPROJECT).elf
	$(ELF2BIN) -O ihex $< $@


DEPS = $(OBJECTS:.o=.d) $(SYS_OBJECTS:.o=.d)
-include $(DEPS)

clean :
	rm -rf BUILD/*
