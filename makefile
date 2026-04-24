TARGET = GpuMMIOFix
CC = gcc
LD = ld.bfd
OBJCOPY = objcopy

CFLAGS = -I/usr/include/efi -I/usr/include/efi/x86_64 \
         -fpic -ffreestanding -fno-stack-protector -fno-stack-check \
         -fshort-wchar -mno-red-zone -Wall -DEFI_FUNCTION_WRAPPER

LDFLAGS = -nostdlib -znocombreloc -T /usr/lib/elf_x86_64_efi.lds \
          -shared -Bsymbolic

LIBS = -L/usr/lib -lefi -lgnuefi

all: $(TARGET).efi

$(TARGET).o: $(TARGET).c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).so: $(TARGET).o
	$(LD) $(LDFLAGS) /usr/lib/crt0-efi-x86_64.o $< -o $@ $(LIBS)

$(TARGET).efi: $(TARGET).so
	$(OBJCOPY) --input-target elf64-x86-64 --output-target efi-app-x86_64 \
	           -j .text -j .sdata -j .data -j .dynamic \
	           -j .dynsym -j .reloc $< $@

clean:
	rm -f *.o *.so *.efi

.PHONY: all clean