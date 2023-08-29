//
// Created by afik_cohen on 8/6/20.
//

#ifndef LED_MARQUEE_FONTS_H
#define LED_MARQUEE_FONTS_H

// This nice 5x7 font from here...
// http://sunge.awardspace.com/glcd-sd/node4.html

// Font details:
// 1) Each char is fixed 5x7 pixels.
// 2) Each byte is one column.
// 3) Columns are left to right order, leftmost byte is leftmost column of pixels.
// 4) Each column is 8 bits high.
// 5) Bit #7 is top line of char, Bit #1 is bottom.
// 6) Bit #0 is always 0, becuase this pin is used as serial input and setting to 1 would enable the pull-up.

// defines ascii characters 0x20-0x7F (32-127)
// PROGMEM after variable name as per https://www.arduino.cc/en/Reference/PROGMEM

#define FONTSTD_WIDTH 5
#define HEART_WIDTH 7
#define ASCII_OFFSET 0x20    // ASSCI code of 1st char in font array
// Max chars a single panel can display at once
#define MAX_CHARS_PER_PANEL COLUMNS_PER_PANEL / (FONTSTD_WIDTH + INTERCHAR_SPACE)


struct Font {
    int width;
    const uint* data;
};

const uint FontStd5x7[]
PROGMEM = {
        0x00, 0x00, 0x00, 0x00, 0x00,//
        0x00, 0x00, 0xfa, 0x00, 0x00,// !
        0x00, 0xe0, 0x00, 0xe0, 0x00,// "
        0x28, 0xfe, 0x28, 0xfe, 0x28,// #
        0x24, 0x54, 0xfe, 0x54, 0x48,// $
        0xc4, 0xc8, 0x10, 0x26, 0x46,// %
        0x6c, 0x92, 0xaa, 0x44, 0x0a,// &
        0x00, 0xa0, 0xc0, 0x00, 0x00,// '
        0x00, 0x38, 0x44, 0x82, 0x00,// (
        0x00, 0x82, 0x44, 0x38, 0x00,// )
        0x10, 0x54, 0x38, 0x54, 0x10,// *
        0x10, 0x10, 0x7c, 0x10, 0x10,// +
        0x00, 0x0a, 0x0c, 0x00, 0x00,// ,
        0x10, 0x10, 0x10, 0x10, 0x10,// -
        0x00, 0x06, 0x06, 0x00, 0x00,// .
        0x04, 0x08, 0x10, 0x20, 0x40,// /
        0x7c, 0x8a, 0x92, 0xa2, 0x7c,// 0
        0x00, 0x42, 0xfe, 0x02, 0x00,// 1
        0x42, 0x86, 0x8a, 0x92, 0x62,// 2
        0x84, 0x82, 0xa2, 0xd2, 0x8c,// 3
        0x18, 0x28, 0x48, 0xfe, 0x08,// 4
        0xe4, 0xa2, 0xa2, 0xa2, 0x9c,// 5
        0x3c, 0x52, 0x92, 0x92, 0x0c,// 6
        0x80, 0x8e, 0x90, 0xa0, 0xc0,// 7
        0x6c, 0x92, 0x92, 0x92, 0x6c,// 8
        0x60, 0x92, 0x92, 0x94, 0x78,// 9
        0x00, 0x6c, 0x6c, 0x00, 0x00,// :
        0x00, 0x6a, 0x6c, 0x00, 0x00,// ;
        0x00, 0x10, 0x28, 0x44, 0x82,// <
        0x28, 0x28, 0x28, 0x28, 0x28,// =
        0x82, 0x44, 0x28, 0x10, 0x00,// >
        0x40, 0x80, 0x8a, 0x90, 0x60,// ?
        0x4c, 0x92, 0x9e, 0x82, 0x7c,// @
        0x7e, 0x88, 0x88, 0x88, 0x7e,// A
        0xfe, 0x92, 0x92, 0x92, 0x6c,// B
        0x7c, 0x82, 0x82, 0x82, 0x44,// C
        0xfe, 0x82, 0x82, 0x44, 0x38,// D
        0xfe, 0x92, 0x92, 0x92, 0x82,// E
        0xfe, 0x90, 0x90, 0x80, 0x80,// F
        0x7c, 0x82, 0x82, 0x8a, 0x4c,// G
        0xfe, 0x10, 0x10, 0x10, 0xfe,// H
        0x00, 0x82, 0xfe, 0x82, 0x00,// I
        0x04, 0x02, 0x82, 0xfc, 0x80,// J
        0xfe, 0x10, 0x28, 0x44, 0x82,// K
        0xfe, 0x02, 0x02, 0x02, 0x02,// L
        0xfe, 0x40, 0x20, 0x40, 0xfe,// M
        0xfe, 0x20, 0x10, 0x08, 0xfe,// N
        0x7c, 0x82, 0x82, 0x82, 0x7c,// O
        0xfe, 0x90, 0x90, 0x90, 0x60,// P
        0x7c, 0x82, 0x8a, 0x84, 0x7a,// Q
        0xfe, 0x90, 0x98, 0x94, 0x62,// R
        0x62, 0x92, 0x92, 0x92, 0x8c,// S
        0x80, 0x80, 0xfe, 0x80, 0x80,// T
        0xfc, 0x02, 0x02, 0x02, 0xfc,// U
        0xf8, 0x04, 0x02, 0x04, 0xf8,// V
        0xfe, 0x04, 0x18, 0x04, 0xfe,// W
        0xc6, 0x28, 0x10, 0x28, 0xc6,// X
        0xc0, 0x20, 0x1e, 0x20, 0xc0,// Y
        0x86, 0x8a, 0x92, 0xa2, 0xc2,// Z
        0x00, 0x00, 0xfe, 0x82, 0x82,// [
        0x40, 0x20, 0x10, 0x08, 0x04,// (backslash)
        0x82, 0x82, 0xfe, 0x00, 0x00,// ]
        0x20, 0x40, 0x80, 0x40, 0x20,// ^
        0x02, 0x02, 0x02, 0x02, 0x02,// _
        0x00, 0x80, 0x40, 0x20, 0x00,// `
        0x04, 0x2a, 0x2a, 0x2a, 0x1e,// a
        0xfe, 0x12, 0x22, 0x22, 0x1c,// b
        0x1c, 0x22, 0x22, 0x22, 0x04,// c
        0x1c, 0x22, 0x22, 0x12, 0xfe,// d
        0x1c, 0x2a, 0x2a, 0x2a, 0x18,// e
        0x10, 0x7e, 0x90, 0x80, 0x40,// f
        0x10, 0x28, 0x2a, 0x2a, 0x3c,// g
        0xfe, 0x10, 0x20, 0x20, 0x1e,// h
        0x00, 0x22, 0xbe, 0x02, 0x00,// i
        0x04, 0x02, 0x22, 0xbc, 0x00,// j
        0x00, 0xfe, 0x08, 0x14, 0x22,// k
        0x00, 0x82, 0xfe, 0x02, 0x00,// l
        0x3e, 0x20, 0x18, 0x20, 0x1e,// m
        0x3e, 0x10, 0x20, 0x20, 0x1e,// n
        0x1c, 0x22, 0x22, 0x22, 0x1c,// o
        0x3e, 0x28, 0x28, 0x28, 0x10,// p
        0x10, 0x28, 0x28, 0x18, 0x3e,// q
        0x3e, 0x10, 0x20, 0x20, 0x10,// r
        0x12, 0x2a, 0x2a, 0x2a, 0x04,// s
        0x20, 0xfc, 0x22, 0x02, 0x04,// t
        0x3c, 0x02, 0x02, 0x04, 0x3e,// u
        0x38, 0x04, 0x02, 0x04, 0x38,// v
        0x3c, 0x02, 0x0c, 0x02, 0x3c,// w
        0x22, 0x14, 0x08, 0x14, 0x22,// x
        0x30, 0x0a, 0x0a, 0x0a, 0x3c,// y
        0x22, 0x26, 0x2a, 0x32, 0x22,// z
        0x00, 0x10, 0x6c, 0x82, 0x00,// {
        0x00, 0x00, 0xfe, 0x00, 0x00,// |
        0x00, 0x82, 0x6c, 0x10, 0x00,// }
        0x10, 0x10, 0x54, 0x38, 0x10,// ~
//      0x10, 0x38, 0x54, 0x10, 0x10,//  //HEART
        0b01110000,
        0b11111000,
        0b01111100,
        0b00111110,
        0b01111100,
        0b11111000,
        0b01110000
};

Font fontStd = {FONTSTD_WIDTH, FontStd5x7};

//0b01110000,
//0b11111100,
//0b01111110,
//0b11111100,
//0b01110000

// A nice arcade font from...
// http://jared.geek.nz/2014/jan/custom-fonts-for-microcontrollers

#define FONTCHONK_WIDTH 8

const uint FontChonk[]
PROGMEM = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,//
        0x06, 0x06, 0x30, 0x30, 0x60, 0xc0, 0xc0, 0x00,// !
        0xe0, 0xe0, 0x00, 0xe0, 0xe0, 0x00, 0x00, 0x00,// "
        0x28, 0xfe, 0xfe, 0x28, 0xfe, 0xfe, 0x28, 0x00,// #
        0xf6, 0xf6, 0xd6, 0xd6, 0xd6, 0xde, 0xde, 0x00,// $
        0xc6, 0xce, 0x1c, 0x38, 0x70, 0xe6, 0xc6, 0x00,// %
        0xfe, 0xfe, 0xd6, 0xc6, 0x16, 0x1e, 0x1e, 0x00,// &
        0x00, 0x00, 0xe0, 0xe0, 0x00, 0x00, 0x00, 0x00,// '
        0xfe, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,// (
        0x00, 0xfe, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00,// )
        0x6c, 0x10, 0xfe, 0xfe, 0xfe, 0x10, 0x6c, 0x00,// *
        0x10, 0x10, 0x7c, 0x10, 0x10, 0x00, 0x00, 0x00,// +
        0b00001100,
        0b00001110,
        0b00000000,
        0b00000000,
        0b00000000,
        0b00000000,
        0b00000000,
        0b00000000,// ,
        0x10, 0x10, 0x10, 0x10, 0x10, 0x00, 0x00, 0x00,// -
        0x06, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,// .
        0x0e, 0x38, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00,// /
        0xfe, 0xfe, 0xc6, 0xc6, 0xc6, 0xfe, 0xfe, 0x00,// 0
        0x06, 0x66, 0x66, 0xfe, 0xfe, 0x06, 0x06, 0x00,// 1
        0xde, 0xde, 0xd6, 0xd6, 0xd6, 0xf6, 0xf6, 0x00,// 2
        0xc6, 0xc6, 0xd6, 0xd6, 0xd6, 0xfe, 0xfe, 0x00,// 3
        0xf8, 0xf8, 0x18, 0x18, 0x18, 0x7e, 0x7e, 0x00,// 4
        0xf6, 0xf6, 0xd6, 0xd6, 0xd6, 0xde, 0xde, 0x00,// 5
        0xfe, 0xfe, 0x36, 0x36, 0x36, 0x3e, 0x3e, 0x00,// 6
        0xc2, 0xc6, 0xce, 0xdc, 0xf8, 0xf0, 0xe0, 0x00,// 7
        0xfe, 0xfe, 0xd6, 0xd6, 0xd6, 0xfe, 0xfe, 0x00,// 8
        0xf8, 0xf8, 0xd8, 0xd8, 0xd8, 0xfe, 0xfe, 0x00,// 9
        0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,// :
        0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,// ;
        0x10, 0x28, 0x44, 0x44, 0x00, 0x00, 0x00, 0x00,// <
        0x28, 0x28, 0x28, 0x28, 0x28, 0x00, 0x00, 0x00,// =
        0x44, 0x44, 0x28, 0x10, 0x00, 0x00, 0x00, 0x00,// >
        0xc0, 0xc0, 0xda, 0xda, 0xd0, 0xf0, 0xf0, 0x00,// ?
        0xfe, 0xfe, 0xc6, 0xf6, 0xd6, 0xf6, 0xf6, 0x00,// @
        0xfe, 0xfe, 0xd8, 0xd8, 0xd8, 0xfe, 0xfe, 0x00,// A
        0xfe, 0xfe, 0xd6, 0xd6, 0xf6, 0x7e, 0x3e, 0x00,// B
        0xfe, 0xfe, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0x00,// C
        0xfe, 0xfe, 0xc6, 0xc6, 0xe6, 0x7e, 0x3e, 0x00,// D
        0xfe, 0xfe, 0xd6, 0xd6, 0xd6, 0xd6, 0xd6, 0x00,// E
        0xfe, 0xfe, 0xd0, 0xd0, 0xd0, 0xc0, 0xc0, 0x00,// F
        0xfe, 0xfe, 0xc6, 0xc6, 0xd6, 0xde, 0xde, 0x00,// G
        0xfe, 0xfe, 0x18, 0x18, 0x18, 0xfe, 0xfe, 0x00,// H
        0xc6, 0xc6, 0xfe, 0xfe, 0xc6, 0xc6, 0xc6, 0x00,// I
        0x06, 0x06, 0x06, 0x06, 0x06, 0xfe, 0xfc, 0x00,// J
        0xfe, 0xfe, 0x18, 0x18, 0x78, 0xfe, 0x9e, 0x00,// K
        0xfe, 0xfe, 0x06, 0x06, 0x06, 0x06, 0x06, 0x00,// L
        0xfe, 0xfe, 0xc0, 0x60, 0xc0, 0xfe, 0xfe, 0x00,// M
        0xfe, 0xfe, 0x70, 0x38, 0x1c, 0xfe, 0xfe, 0x00,// N
        0xfe, 0xfe, 0xc6, 0xc6, 0xc6, 0xfe, 0xfe, 0x00,// O
        0xfe, 0xfe, 0xd8, 0xd8, 0xd8, 0xf8, 0xf8, 0x00,// P
        0xfe, 0xfe, 0xc6, 0xce, 0xce, 0xfe, 0xfe, 0x00,// Q
        0xfe, 0xfe, 0xd8, 0xdc, 0xde, 0xfe, 0xfa, 0x00,// R
        0xf6, 0xf6, 0xd6, 0xd6, 0xd6, 0xde, 0xde, 0x00,// S
        0xc0, 0xc0, 0xfe, 0xfe, 0xc0, 0xc0, 0xc0, 0x00,// T
        0xfe, 0xfe, 0x06, 0x06, 0x06, 0xfe, 0xfe, 0x00,// U
        0xf8, 0xfc, 0x0e, 0x06, 0x0e, 0xfc, 0xf8, 0x00,// V
        0xfc, 0xfe, 0x06, 0x0c, 0x06, 0xfe, 0xfc, 0x00,// W
        0xee, 0xfe, 0x38, 0x10, 0x38, 0xfe, 0xee, 0x00,// X
        0xe0, 0xf0, 0x3e, 0x1e, 0x3e, 0xf0, 0xe0, 0x00,// Y
        0xce, 0xde, 0xd6, 0xd6, 0xd6, 0xf6, 0xe6, 0x00,// Z
        0xfe, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,// [
        0xe0, 0x38, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00,// backslash(\)
        0x00, 0xfe, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00,// ]
        0x00, 0xfe, 0x02, 0xfe, 0x00, 0x00, 0x00, 0x00,// ^
        0b00000110,
        0b00000110,
        0b00000110,
        0b00000110,
        0b00000110,
        0b00000110,
        0b00000110,
        0b00000110,// _
        0x00, 0xfe, 0x02, 0xfe, 0x00, 0x00, 0x00, 0x00,// `
        0xfe, 0xfe, 0xd8, 0xd8, 0xd8, 0xfe, 0xfe, 0x00,// a
        0xfe, 0xfe, 0xd6, 0xd6, 0xf6, 0x7e, 0x3e, 0x00,// b
        0xfe, 0xfe, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0x00,// c
        0xfe, 0xfe, 0xc6, 0xc6, 0xe6, 0x7e, 0x3e, 0x00,// d
        0xfe, 0xfe, 0xd6, 0xd6, 0xd6, 0xd6, 0xd6, 0x00,// e
        0xfe, 0xfe, 0xd0, 0xd0, 0xd0, 0xc0, 0xc0, 0x00,// f
        0xfe, 0xfe, 0xc6, 0xc6, 0xd6, 0xde, 0xde, 0x00,// g
        0xfe, 0xfe, 0x18, 0x18, 0x18, 0xfe, 0xfe, 0x00,// h
        0xc6, 0xc6, 0xfe, 0xfe, 0xc6, 0xc6, 0xc6, 0x00,// i
        0x06, 0x06, 0x06, 0x06, 0x06, 0xfe, 0xfc, 0x00,// j
        0xfe, 0xfe, 0x18, 0x18, 0x78, 0xfe, 0x9e, 0x00,// k
        0xfe, 0xfe, 0x06, 0x06, 0x06, 0x06, 0x06, 0x00,// l
        0xfe, 0xfe, 0xc0, 0x60, 0xc0, 0xfe, 0xfe, 0x00,// m
        0xfe, 0xfe, 0x70, 0x38, 0x1c, 0xfe, 0xfe, 0x00,// n
        0xfe, 0xfe, 0xc6, 0xc6, 0xc6, 0xfe, 0xfe, 0x00,// o
        0xfe, 0xfe, 0xd8, 0xd8, 0xd8, 0xf8, 0xf8, 0x00,// p
        0xfe, 0xfe, 0xc6, 0xce, 0xce, 0xfe, 0xfe, 0x00,// q
        0xfe, 0xfe, 0xd8, 0xdc, 0xde, 0xfe, 0xfa, 0x00,// r
        0xf6, 0xf6, 0xd6, 0xd6, 0xd6, 0xde, 0xde, 0x00,// s
        0xc0, 0xc0, 0xfe, 0xfe, 0xc0, 0xc0, 0xc0, 0x00,// t
        0xfe, 0xfe, 0x06, 0x06, 0x06, 0xfe, 0xfe, 0x00,// u
        0xf8, 0xfc, 0x0e, 0x06, 0x0e, 0xfc, 0xf8, 0x00,// v
        0xfc, 0xfe, 0x06, 0x0c, 0x06, 0xfe, 0xfc, 0x00,// w
        0xee, 0xfe, 0x38, 0x10, 0x38, 0xfe, 0xee, 0x00,// x
        0xe0, 0xf0, 0x3e, 0x1e, 0x3e, 0xf0, 0xe0, 0x00,// y
        0xce, 0xde, 0xd6, 0xd6, 0xd6, 0xf6, 0xe6, 0x00,// z
        0x38, 0xfe, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00,// {
        0b00000000,
        0b11111110,
        0b11111110,
        0b11111110,
        0b11111110,
        0b11111110,
        0b11111110,
        0b00000000,// |
        0x00, 0xfe, 0xfe, 0x38, 0x00, 0x00, 0x00, 0x00,// }
        0x00, 0xfe, 0x02, 0xfe, 0x00, 0x00, 0x00, 0x00,// ~
};

Font fontChonk = {FONTCHONK_WIDTH, FontChonk};

#endif //LED_MARQUEE_FONTS_H
