const unsigned char font[96][6] = {
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, //
        {0x2e, 0x00, 0x00, 0x00, 0x00, 0x00}, // !
        {0x06, 0x00, 0x06, 0x00, 0x00, 0x00}, // "
        {0x14, 0x3e, 0x14, 0x3e, 0x14, 0x00}, // #
        {0x04, 0x2a, 0x3e, 0x2a, 0x10, 0x00}, // $
        {0x22, 0x10, 0x08, 0x04, 0x22, 0x00}, // %
        {0x14, 0x2a, 0x2a, 0x2c, 0x10, 0x28}, // &
        {0x06, 0x00, 0x00, 0x00, 0x00, 0x00}, // '
        {0x1c, 0x22, 0x00, 0x00, 0x00, 0x00}, // (
        {0x22, 0x1c, 0x00, 0x00, 0x00, 0x00}, // )
        {0x14, 0x08, 0x14, 0x00, 0x00, 0x00}, // *
        {0x08, 0x1c, 0x08, 0x00, 0x00, 0x00}, // +
        {0x60, 0x00, 0x00, 0x00, 0x00, 0x00}, // ,
        {0x08, 0x08, 0x08, 0x00, 0x00, 0x00}, // -
        {0x20, 0x00, 0x00, 0x00, 0x00, 0x00}, // .
        {0x30, 0x0c, 0x02, 0x00, 0x00, 0x00}, // /
        {0x1c, 0x22, 0x22, 0x22, 0x1e, 0x00}, // 0
        {0x02, 0x3e, 0x00, 0x00, 0x00, 0x00}, // 1
        {0x32, 0x2a, 0x2a, 0x24, 0x00, 0x00}, // 2
        {0x2a, 0x2a, 0x2a, 0x16, 0x00, 0x00}, // 3
        {0x0e, 0x10, 0x10, 0x3e, 0x10, 0x00}, // 4
        {0x2e, 0x2a, 0x2a, 0x12, 0x00, 0x00}, // 5
        {0x3c, 0x2a, 0x2a, 0x2a, 0x12, 0x00}, // 6
        {0x06, 0x02, 0x22, 0x12, 0x0e, 0x00}, // 7
        {0x14, 0x2a, 0x2a, 0x2a, 0x16, 0x00}, // 8
        {0x04, 0x2a, 0x2a, 0x2a, 0x1e, 0x00}, // 9
        {0x24, 0x00, 0x00, 0x00, 0x00, 0x00}, // :
        {0x64, 0x00, 0x00, 0x00, 0x00, 0x00}, // ;
        {0x08, 0x14, 0x22, 0x00, 0x00, 0x00}, // <
        {0x14, 0x14, 0x14, 0x00, 0x00, 0x00}, // =
        {0x22, 0x14, 0x08, 0x00, 0x00, 0x00}, // >
        {0x02, 0x2a, 0x0a, 0x04, 0x00, 0x00}, // ?
        {0x3c, 0x02, 0x1a, 0x2a, 0x22, 0x1e}, // @
        {0x3c, 0x12, 0x12, 0x12, 0x3e, 0x00}, // A
        {0x3c, 0x2a, 0x2a, 0x2e, 0x10, 0x00}, // B
        {0x1c, 0x22, 0x22, 0x22, 0x00, 0x00}, // C
        {0x3c, 0x22, 0x22, 0x22, 0x1c, 0x00}, // D
        {0x3c, 0x2a, 0x2a, 0x2a, 0x00, 0x00}, // E
        {0x3c, 0x12, 0x12, 0x12, 0x00, 0x00}, // F
        {0x3c, 0x22, 0x22, 0x2a, 0x1a, 0x00}, // G
        {0x3e, 0x08, 0x08, 0x3e, 0x00, 0x00}, // H
        {0x22, 0x3e, 0x22, 0x00, 0x00, 0x00}, // I
        {0x30, 0x22, 0x22, 0x1e, 0x00, 0x00}, // J
        {0x3e, 0x08, 0x0c, 0x32, 0x00, 0x00}, // K
        {0x3e, 0x20, 0x20, 0x20, 0x00, 0x00}, // L
        {0x3c, 0x02, 0x02, 0x3c, 0x02, 0x02}, // M
        {0x3c, 0x02, 0x02, 0x02, 0x3e, 0x00}, // N
        {0x1c, 0x22, 0x22, 0x22, 0x1e, 0x00}, // O
        {0x3c, 0x12, 0x12, 0x12, 0x0e, 0x00}, // P
        {0x1c, 0x22, 0x22, 0x62, 0x1e, 0x00}, // Q
        {0x3c, 0x12, 0x12, 0x32, 0x0e, 0x00}, // R
        {0x24, 0x2a, 0x2a, 0x12, 0x00, 0x00}, // S
        {0x02, 0x02, 0x3e, 0x02, 0x02, 0x00}, // T
        {0x1e, 0x20, 0x20, 0x20, 0x1e, 0x00}, // U
        {0x0e, 0x10, 0x20, 0x10, 0x0e, 0x00}, // V
        {0x3e, 0x20, 0x20, 0x1e, 0x20, 0x20}, // W
        {0x36, 0x08, 0x08, 0x36, 0x00, 0x00}, // X
        {0x26, 0x28, 0x28, 0x1e, 0x00, 0x00}, // Y
        {0x32, 0x2a, 0x2a, 0x26, 0x00, 0x00}, // Z
        {0x3e, 0x22, 0x00, 0x00, 0x00, 0x00}, // [
        {0x02, 0x0c, 0x30, 0x00, 0x00, 0x00}, // "\"
        {0x22, 0x3e, 0x00, 0x00, 0x00, 0x00}, // ]
        {0x04, 0x02, 0x04, 0x00, 0x00, 0x00}, // ^
        {0x20, 0x20, 0x20, 0x00, 0x00, 0x00}, // _
        {0x02, 0x04, 0x00, 0x00, 0x00, 0x00}, // `
        {0x3c, 0x12, 0x12, 0x12, 0x3e, 0x00}, // a
        {0x3c, 0x2a, 0x2a, 0x2e, 0x10, 0x00}, // b
        {0x1c, 0x22, 0x22, 0x22, 0x00, 0x00}, // c
        {0x3c, 0x22, 0x22, 0x22, 0x1c, 0x00}, // d
        {0x3c, 0x2a, 0x2a, 0x2a, 0x00, 0x00}, // e
        {0x3c, 0x12, 0x12, 0x12, 0x00, 0x00}, // f
        {0x3c, 0x22, 0x22, 0x2a, 0x1a, 0x00}, // g
        {0x3e, 0x08, 0x08, 0x3e, 0x00, 0x00}, // h
        {0x22, 0x3e, 0x22, 0x00, 0x00, 0x00}, // i
        {0x30, 0x22, 0x22, 0x1e, 0x00, 0x00}, // j
        {0x3e, 0x08, 0x0c, 0x32, 0x00, 0x00}, // k
        {0x3e, 0x20, 0x20, 0x20, 0x00, 0x00}, // l
        {0x3c, 0x02, 0x02, 0x3c, 0x02, 0x02}, // m
        {0x3c, 0x02, 0x02, 0x02, 0x3e, 0x00}, // n
        {0x1c, 0x22, 0x22, 0x22, 0x1e, 0x00}, // o
        {0x3c, 0x12, 0x12, 0x12, 0x0e, 0x00}, // p
        {0x1c, 0x22, 0x22, 0x62, 0x1e, 0x00}, // q
        {0x3c, 0x12, 0x12, 0x32, 0x0e, 0x00}, // r
        {0x24, 0x2a, 0x2a, 0x12, 0x00, 0x00}, // s
        {0x02, 0x02, 0x3e, 0x02, 0x02, 0x00}, // t
        {0x1e, 0x20, 0x20, 0x20, 0x1e, 0x00}, // u
        {0x0e, 0x10, 0x20, 0x10, 0x0e, 0x00}, // v
        {0x3e, 0x20, 0x20, 0x1e, 0x20, 0x20}, // w
        {0x36, 0x08, 0x08, 0x36, 0x00, 0x00}, // x
        {0x26, 0x28, 0x28, 0x1e, 0x00, 0x00}, // y
        {0x32, 0x2a, 0x2a, 0x26, 0x00, 0x00}, // z
        {0x08, 0x3e, 0x22, 0x00, 0x00, 0x00}, // {
        {0x3e, 0x00, 0x00, 0x00, 0x00, 0x00}, // |
        {0x22, 0x3e, 0x08, 0x00, 0x00, 0x00}, // }
        {0x04, 0x02, 0x02, 0x00, 0x00, 0x00}, // ~
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}
};