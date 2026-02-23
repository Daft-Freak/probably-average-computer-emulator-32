#include <array>

#include "KeyMap.h"

static const ATScancode scancodeMap[]
{
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,

    ATScancode::A,
    ATScancode::B,
    ATScancode::C,
    ATScancode::D,
    ATScancode::E,
    ATScancode::F,
    ATScancode::G,
    ATScancode::H,
    ATScancode::I,
    ATScancode::J,
    ATScancode::K,
    ATScancode::L,
    ATScancode::M,
    ATScancode::N,
    ATScancode::O,
    ATScancode::P,
    ATScancode::Q,
    ATScancode::R,
    ATScancode::S,
    ATScancode::T,
    ATScancode::U,
    ATScancode::V,
    ATScancode::W,
    ATScancode::X,
    ATScancode::Y,
    ATScancode::Z,
    
    ATScancode::_1,
    ATScancode::_2,
    ATScancode::_3,
    ATScancode::_4,
    ATScancode::_5,
    ATScancode::_6,
    ATScancode::_7,
    ATScancode::_8,
    ATScancode::_9,
    ATScancode::_0,

    ATScancode::Return,
    ATScancode::Escape,
    ATScancode::Backspace,
    ATScancode::Tab,
    ATScancode::Space,

    ATScancode::Minus,
    ATScancode::Equals,
    ATScancode::LeftBracket,
    ATScancode::RightBracket,
    ATScancode::Backslash,
    ATScancode::Backslash, // same key
    ATScancode::Semicolon,
    ATScancode::Apostrophe,
    ATScancode::Grave,
    ATScancode::Comma,
    ATScancode::Period,
    ATScancode::Slash,

    ATScancode::CapsLock,

    ATScancode::F1,
    ATScancode::F2,
    ATScancode::F3,
    ATScancode::F4,
    ATScancode::F5,
    ATScancode::F6,
    ATScancode::F7,
    ATScancode::F8,
    ATScancode::F9,
    ATScancode::F10,
    ATScancode::F11,
    ATScancode::F12,

    ATScancode::Invalid, // PrintScreen
    ATScancode::ScrollLock,
    ATScancode::Invalid, // Pause
    ATScancode::Insert,
    
    ATScancode::Home,
    ATScancode::PageUp,
    ATScancode::Delete,
    ATScancode::End,
    ATScancode::PageDown,
    ATScancode::Right,
    ATScancode::Left,
    ATScancode::Down,
    ATScancode::Up,

    ATScancode::NumLock,

    ATScancode::KPDivide,
    ATScancode::KPMultiply,
    ATScancode::KPMinus,
    ATScancode::KPPlus,
    ATScancode::KPEnter,
    ATScancode::KP1,
    ATScancode::KP2,
    ATScancode::KP3,
    ATScancode::KP4,
    ATScancode::KP5,
    ATScancode::KP6,
    ATScancode::KP7,
    ATScancode::KP8,
    ATScancode::KP9,
    ATScancode::KP0,
    ATScancode::KPPeriod,

    ATScancode::NonUSBackslash,

    ATScancode::Application,
    ATScancode::Power,

    ATScancode::KPEquals,

    // F13-F24
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,

    // no mapping
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,

    ATScancode::KPComma,
    ATScancode::Invalid,

    ATScancode::International1,
    ATScancode::International2,
    ATScancode::International3,
    ATScancode::International4,
    ATScancode::International5,
    ATScancode::International6,
    ATScancode::Invalid, // ...7
    ATScancode::Invalid, // ...8
    ATScancode::Invalid, // ...9
    ATScancode::Lang1,
    ATScancode::Lang2,
    ATScancode::Lang3,
    ATScancode::Lang4,
    ATScancode::Lang5,

    // ... some media keys
};

static const ATScancode modMap[]
{
    ATScancode::LeftCtrl,
    ATScancode::LeftShift,
    ATScancode::LeftAlt,
    ATScancode::LeftGUI,
    ATScancode::RightCtrl,
    ATScancode::RightShift,
    ATScancode::RightAlt,
    ATScancode::RightGUI,
};

ATScancode map_hid_key(uint8_t key)
{
    if(key >= std::size(scancodeMap))
        return ATScancode::Invalid;

    return scancodeMap[key];
}

ATScancode map_hid_mod(unsigned bit)
{
    if(bit >= std::size(modMap))
        return ATScancode::Invalid;

    return modMap[bit];
}