// The colours of the White and Dark looks, as plain numbers with no UI framework, so the contrast rule can be
// checked by a swiftc check outside Xcode. `Theme` turns them into SwiftUI colours.
// Rule (owner, for elderly readers): text is at least 7:1 on its background in White, at least 4.5:1 in Dark.
// White follows the owner's site: a light grey page, white cards, blue for actions and links.

enum ThemeMode: CaseIterable { case white, dark }

enum ThemeToken: CaseIterable {
    // Surfaces.
    case background                      // the page
    case card, input, tabBar, navBar     // what sits on the page
    case formPage, formRow               // behind a Form's rows, and the rows themselves
    case chip                            // small filled chips and toasts
    case pillFill, pillBorder            // the unselected pill
    // Text.
    case text, secondary, hairline       // body text, every quieter line, and non-essential lines only
    case link                            // links, text highlights, the selected tab
    // The main button, and your own chat bubble.
    case primaryFill, primaryLabel
    case bubbleFill, bubbleLabel
    // Meanings, each with its own name. "Icon" is the colour for fills, icons and borders; the plain name is for text.
    case positive, positiveIcon, positiveLabel   // Keep, Yes, Done. Its filled button is positiveIcon with positiveLabel
    case negative, negativeIcon          // Wrong, No
    case warning, warningIcon            // errors and destructive actions
    case medicine, medicineIcon          // medicine marks
    case caution, cautionIcon, cautionFill   // "please check": flagged fields and notices
    case health, healthIcon, healthFill  // health notices
    case info, infoIcon                  // overnight work
    case highlight                       // a highlighted line in a transcript
    case badgeFill, badgeLabel           // the counts on section titles
    case record, recordText              // the red record circle, and "Record" under it
    case swipe, swipeLabel               // neutral swipe action
    // The map's branches: fills, lines and points only, never text.
    case mapPeople, mapTopics, mapPromises, mapDecisions, mapQuestions
}

enum ThemePalette {
    /// 0xRRGGBB for a colour in a mode. One line per colour.
    static func hex(_ token: ThemeToken, _ mode: ThemeMode) -> UInt32 {
        switch (token, mode) {
        case (.background, .white): return 0xF5F5F7
        case (.background, .dark): return 0x000000
        case (.card, .white): return 0xFFFFFF
        case (.card, .dark): return 0x1D1D1F
        case (.input, .white): return 0xFFFFFF
        case (.input, .dark): return 0x1D1D1F
        case (.tabBar, .white): return 0xFFFFFF
        case (.tabBar, .dark): return 0x000000
        case (.navBar, .white): return 0xFFFFFF
        case (.navBar, .dark): return 0x000000
        case (.formPage, .white): return 0xF5F5F7
        case (.formPage, .dark): return 0x000000
        case (.formRow, .white): return 0xFFFFFF
        case (.formRow, .dark): return 0x1D1D1F
        case (.chip, .white): return 0xE8E8ED
        case (.chip, .dark): return 0x2C2C2E
        case (.pillFill, .white): return 0xFFFFFF
        case (.pillFill, .dark): return 0x2C2C2E
        case (.pillBorder, .white): return 0xD2D2D7
        case (.pillBorder, .dark): return 0x48484A
        case (.text, .white): return 0x1D1D1F
        case (.text, .dark): return 0xF5F5F7
        case (.secondary, .white): return 0x515154
        case (.secondary, .dark): return 0xA1A1A6
        case (.hairline, .white): return 0x86868B
        case (.hairline, .dark): return 0x636366
        case (.link, .white): return 0x0051A3
        case (.link, .dark): return 0x2997FF
        case (.primaryFill, .white): return 0x0066CC
        case (.primaryFill, .dark): return 0xF5F5F7
        case (.primaryLabel, .white): return 0xFFFFFF
        case (.primaryLabel, .dark): return 0x000000
        case (.bubbleFill, .white): return 0x0051A3
        case (.bubbleFill, .dark): return 0xF5F5F7
        case (.bubbleLabel, .white): return 0xFFFFFF
        case (.bubbleLabel, .dark): return 0x000000
        case (.positive, .white): return 0x175F2D
        case (.positive, .dark): return 0x30D158
        case (.positiveIcon, .white): return 0x34C759
        case (.positiveIcon, .dark): return 0x30D158
        case (.positiveLabel, .white): return 0x1D1D1F
        case (.positiveLabel, .dark): return 0x000000
        case (.negative, .white): return 0x9F211A
        case (.negative, .dark): return 0xFF6961
        case (.negativeIcon, .white): return 0xFF3B30
        case (.negativeIcon, .dark): return 0xFF453A
        case (.warning, .white): return 0x9F211A
        case (.warning, .dark): return 0xFF6961
        case (.warningIcon, .white): return 0xFF3B30
        case (.warningIcon, .dark): return 0xFF453A
        case (.medicine, .white): return 0x9F211A
        case (.medicine, .dark): return 0xFF6961
        case (.medicineIcon, .white): return 0xFF3B30
        case (.medicineIcon, .dark): return 0xFF453A
        case (.caution, .white): return 0x7E4400
        case (.caution, .dark): return 0xFFB340
        case (.cautionIcon, .white): return 0xFF9500
        case (.cautionIcon, .dark): return 0xFF9F0A
        case (.cautionFill, .white): return 0xFFF3D6
        case (.cautionFill, .dark): return 0x3A2C10
        case (.health, .white): return 0x7E4400
        case (.health, .dark): return 0xFFB340
        case (.healthIcon, .white): return 0xFF9500
        case (.healthIcon, .dark): return 0xFF9F0A
        case (.healthFill, .white): return 0xFFF3D6
        case (.healthFill, .dark): return 0x3A2C10
        case (.info, .white): return 0x4442B5
        case (.info, .dark): return 0xC9C8FF
        case (.infoIcon, .white): return 0x5856D6
        case (.infoIcon, .dark): return 0x7D7AFF
        case (.highlight, .white): return 0xFFF3D6
        case (.highlight, .dark): return 0x3A2C10
        case (.badgeFill, _): return 0x9F211A
        case (.badgeLabel, _): return 0xFFFFFF
        case (.record, _): return 0xFF3B30
        case (.recordText, .white): return 0x9F211A
        case (.recordText, .dark): return 0xFF6961
        case (.swipe, .white): return 0x0051A3
        case (.swipe, .dark): return 0x0060C0
        case (.swipeLabel, _): return 0xFFFFFF
        case (.mapPeople, .white): return 0x0066CC
        case (.mapPeople, .dark): return 0x2997FF
        case (.mapTopics, .white): return 0x5856D6
        case (.mapTopics, .dark): return 0x7D7AFF
        case (.mapPromises, .white): return 0x34C759
        case (.mapPromises, .dark): return 0x30D158
        case (.mapDecisions, .white): return 0xFF9500
        case (.mapDecisions, .dark): return 0xFF9F0A
        case (.mapQuestions, .white): return 0x30B0C7
        case (.mapQuestions, .dark): return 0x40C8E0
        }
    }

    /// Every text colour on every fill it is drawn on. A new pair in the app goes here first.
    static let textPairs: [(text: ThemeToken, on: ThemeToken)] = {
        var pairs: [(text: ThemeToken, on: ThemeToken)] = [
            (.bubbleLabel, .bubbleFill), (.positiveLabel, .positiveIcon),
            (.badgeLabel, .badgeFill), (.swipeLabel, .swipe),
            (.text, .chip),
            (.text, .cautionFill), (.secondary, .cautionFill), (.caution, .cautionFill),
            (.text, .healthFill), (.secondary, .healthFill), (.health, .healthFill),
            (.text, .highlight), (.secondary, .highlight), (.link, .highlight),
            (.text, .tabBar), (.secondary, .tabBar), (.link, .tabBar), (.recordText, .tabBar),
            (.text, .navBar), (.link, .navBar),
            (.text, .input), (.secondary, .input),
            (.text, .pillFill), (.positive, .pillFill), (.negative, .pillFill), (.warning, .pillFill),
        ]
        for fg in [ThemeToken.text, .secondary, .link, .warning, .medicine, .caution, .health, .info, .positive, .negative, .recordText] {
            for bg in [ThemeToken.background, .card, .formRow, .formPage] { pairs.append((fg, bg)) }
        }
        return pairs
    }()

    /// Semibold 17 pt or larger counts as large text, at 4.5:1 in both modes. Only the main button: its label is
    /// always that size or larger (PillButtonStyle).
    static let largeTextPairs: [(text: ThemeToken, on: ThemeToken)] = [(.primaryLabel, .primaryFill)]
    static let minimumLargeContrast = 4.5

    static func minimumContrast(_ mode: ThemeMode) -> Double { mode == .white ? 7 : 4.5 }

    /// WCAG relative luminance of 0xRRGGBB.
    static func luminance(_ hex: UInt32) -> Double {
        func channel(_ shift: UInt32) -> Double {
            let c = Double((hex >> shift) & 0xFF) / 255
            if c <= 0.03928 { return c / 12.92 }
            // (c+0.055)/1.055 to the power 2.4, without Foundation's pow.
            return power2point4((c + 0.055) / 1.055)
        }
        return 0.2126 * channel(16) + 0.7152 * channel(8) + 0.0722 * channel(0)
    }

    /// x^2.4 = x^2 * x^0.4, and x^0.4 = (x^(1/5))^2. The fifth root by Newton: x is in (0, 1].
    private static func power2point4(_ x: Double) -> Double {
        var r = x < 0.5 ? 0.5 : x
        for _ in 0..<60 { r -= (r * r * r * r * r - x) / (5 * r * r * r * r) }
        return x * x * r * r
    }

    /// WCAG contrast ratio, 1...21.
    static func contrast(_ a: UInt32, _ b: UInt32) -> Double {
        let (la, lb) = (luminance(a), luminance(b))
        return (max(la, lb) + 0.05) / (min(la, lb) + 0.05)
    }
}
