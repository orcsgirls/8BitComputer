def twosComplement (value, bitLength) :
    return int(bin(value & (2**bitLength - 1)),2)


# Output to match 7 segments for number - Ben Eater wiring
digits = [0x7e, 0x30, 0x6d, 0x79, 0x33, 0x5b, 0x5f, 0x70, 0x7f, 0x7b]

# Empty binary array 
rom = bytearray([0x00] * 2048)

# Loop over digits (see Output Module - Ben Eater)
# Normal numbers - 0 to 255

for value in range(256):
    rom[value] =     digits[ value         % 10]
    rom[value+256] = digits[(value // 10)  % 10]
    rom[value+512] = digits[(value // 100) % 10]

# Normal numbers
# 2's complement - -128 to 128

for value in range(-128, 128):
    addr=twosComplement(value,8)
    rom[addr+1024] = digits[ abs(value)         % 10]
    rom[addr+1280] = digits[(abs(value) // 10)  % 10]
    rom[addr+1536] = digits[(abs(value) // 100) % 10]

    if(value<0):
        rom[addr+1792] = 0x01
    else:
        rom[addr+1792] = 0x00

# Write file
with open("display.bin", "wb") as out_file:
    out_file.write(rom)

