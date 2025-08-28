#include "KeyMappingTest.h"
#include <iostream>
#include <cassert>
#include <windows.h>
#include "KeyInputHandler.h"

namespace KeyMappingTest {

// Extended key mapping test data
struct ExtendedKeyTestCase {
    const char* jsCode;
    WORD expectedVK;
    bool shouldBeExtended;
    const char* description;
};

// Test cases for extended keys that require KEYEVENTF_EXTENDEDKEY
static const ExtendedKeyTestCase extendedKeyTests[] = {
    // Right-side modifier keys
    {"AltRight", VK_RMENU, true, "Right Alt (AltGr)"},
    {"ControlRight", VK_RCONTROL, true, "Right Control"},

    // Navigation keys
    {"ArrowUp", VK_UP, true, "Up Arrow"},
    {"ArrowDown", VK_DOWN, true, "Down Arrow"},
    {"ArrowLeft", VK_LEFT, true, "Left Arrow"},
    {"ArrowRight", VK_RIGHT, true, "Right Arrow"},

    // Editing keys
    {"Insert", VK_INSERT, true, "Insert"},
    {"Delete", VK_DELETE, true, "Delete"},
    {"Home", VK_HOME, true, "Home"},
    {"End", VK_END, true, "End"},
    {"PageUp", VK_PRIOR, true, "Page Up"},
    {"PageDown", VK_NEXT, true, "Page Down"},

    // Numpad keys
    {"NumpadEnter", VK_RETURN, true, "Numpad Enter"},

    // Function keys (extended)
    {"F11", VK_F11, false, "F11 (not extended)"},
    {"F12", VK_F12, false, "F12 (not extended)"},

    // Left-side modifiers (should NOT be extended)
    {"AltLeft", VK_LMENU, false, "Left Alt"},
    {"ControlLeft", VK_LCONTROL, false, "Left Control"},
    {"ShiftLeft", VK_LSHIFT, false, "Left Shift"},
    {"ShiftRight", VK_RSHIFT, false, "Right Shift"},

    // Regular keys (should NOT be extended)
    {"KeyA", 'A', false, "Letter A"},
    {"Digit1", '1', false, "Number 1"},
    {"Space", VK_SPACE, false, "Spacebar"},
};

// Test function for extended key mapping
bool testExtendedKeyMapping() {
    std::cout << "[KeyMappingTest] Testing extended key mapping..." << std::endl;

    bool allTestsPassed = true;
    int testCount = 0;
    int passedCount = 0;

    for (const auto& testCase : extendedKeyTests) {
        testCount++;

        // Map JavaScript code to VK
        WORD vkCode = MapJavaScriptCodeToVK(testCase.jsCode);
        bool isExtended = IsExtendedKey(vkCode);

        bool vkMatches = (vkCode == testCase.expectedVK);
        bool extendedMatches = (isExtended == testCase.shouldBeExtended);

        if (vkMatches && extendedMatches) {
            passedCount++;
            std::cout << "[PASS] " << testCase.description << std::endl;
        } else {
            allTestsPassed = false;
            std::cout << "[FAIL] " << testCase.description << std::endl;
            std::cout << "  Expected: VK=" << testCase.expectedVK
                      << ", Extended=" << (testCase.shouldBeExtended ? "true" : "false") << std::endl;
            std::cout << "  Actual:   VK=" << vkCode
                      << ", Extended=" << (isExtended ? "true" : "false") << std::endl;
        }
    }

    std::cout << "[KeyMappingTest] Results: " << passedCount << "/" << testCount << " tests passed" << std::endl;

    if (allTestsPassed) {
        std::cout << "[KeyMappingTest] All extended key mapping tests PASSED!" << std::endl;
    } else {
        std::cout << "[KeyMappingTest] Some extended key mapping tests FAILED!" << std::endl;
    }

    return allTestsPassed;
}

// Test function for keyboard layout awareness
bool testKeyboardLayoutMapping() {
    std::cout << "[KeyMappingTest] Testing keyboard layout awareness..." << std::endl;

    bool allTestsPassed = true;

    // Test with current layout
    HKL currentLayout = GetKeyboardLayout(0);
    std::cout << "[KeyMappingTest] Current keyboard layout: " << std::hex << currentLayout << std::dec << std::endl;

    // Test key mapping with different layouts (if available)
    // Note: This is a simplified test - in practice you'd want to test with multiple layouts

    // Test that we can get scancodes for different layouts
    WORD vk = 'A';
    UINT scanCode = MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC_EX, currentLayout);

    if (scanCode != 0) {
        std::cout << "[PASS] Successfully mapped VK '" << (char)vk << "' to scan code: " << scanCode << std::endl;
    } else {
        std::cout << "[FAIL] Failed to map VK '" << (char)vk << "' to scan code" << std::endl;
        allTestsPassed = false;
    }

    // Test extended key detection for right control
    vk = VK_RCONTROL;
    bool isExtended = IsExtendedKey(vk);
    if (isExtended) {
        std::cout << "[PASS] VK_RCONTROL correctly identified as extended key" << std::endl;
    } else {
        std::cout << "[FAIL] VK_RCONTROL not identified as extended key" << std::endl;
        allTestsPassed = false;
    }

    // Test extended key detection for left control
    vk = VK_LCONTROL;
    isExtended = IsExtendedKey(vk);
    if (!isExtended) {
        std::cout << "[PASS] VK_LCONTROL correctly identified as non-extended key" << std::endl;
    } else {
        std::cout << "[FAIL] VK_LCONTROL incorrectly identified as extended key" << std::endl;
        allTestsPassed = false;
    }

    return allTestsPassed;
}

// Comprehensive test suite
bool runAllKeyMappingTests() {
    std::cout << "[KeyMappingTest] ===================================" << std::endl;
    std::cout << "[KeyMappingTest] Starting Key Mapping Test Suite" << std::endl;
    std::cout << "[KeyMappingTest] ===================================" << std::endl;

    bool extendedTestsPassed = testExtendedKeyMapping();
    bool layoutTestsPassed = testKeyboardLayoutMapping();

    bool allTestsPassed = extendedTestsPassed && layoutTestsPassed;

    std::cout << "[KeyMappingTest] ===================================" << std::endl;
    if (allTestsPassed) {
        std::cout << "[KeyMappingTest] ALL TESTS PASSED!" << std::endl;
    } else {
        std::cout << "[KeyMappingTest] SOME TESTS FAILED!" << std::endl;
    }
    std::cout << "[KeyMappingTest] ===================================" << std::endl;

    return allTestsPassed;
}

} // namespace KeyMappingTest
