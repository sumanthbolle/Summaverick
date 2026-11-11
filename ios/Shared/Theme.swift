// The one place for colours. Compiled into the app and the widgets. The numbers live in ThemePalette, where the
// contrast rule is checked. In the app the colours follow the Appearance setting; widgets and the Live Activity
// follow the system and pick a fixed mode by their colour scheme, using color(_:_:).
import SwiftUI
import UIKit

enum Theme {
    // The numbers are one line each in ThemePalette. A plain name is for text; "Icon" is for fills, icons and borders.
    // Surfaces.
    static let background = color(.background)
    static let card = color(.card)
    static let input = color(.input)
    static let tabBar = color(.tabBar)
    static let navBar = color(.navBar)
    static let formPage = color(.formPage)
    static let formRow = color(.formRow)
    static let chip = color(.chip)
    static let pillFill = color(.pillFill)
    static let pillBorder = color(.pillBorder)

    // Text.
    static let text = color(.text)
    static let secondary = color(.secondary)
    static let hairline = color(.hairline)
    static let link = color(.link)

    // The main button and your own chat bubble.
    static let primaryFill = color(.primaryFill)
    static let primaryLabel = color(.primaryLabel)
    static let bubbleFill = color(.bubbleFill)
    static let bubbleLabel = color(.bubbleLabel)

    // Keep, Yes, Done.
    static let positive = color(.positive)
    static let positiveIcon = color(.positiveIcon)
    static let positiveLabel = color(.positiveLabel)

    // Wrong, No.
    static let negative = color(.negative)
    static let negativeIcon = color(.negativeIcon)

    // Errors and destructive actions.
    static let warning = color(.warning)
    static let warningIcon = color(.warningIcon)

    // Medicine marks.
    static let medicine = color(.medicine)
    static let medicineIcon = color(.medicineIcon)

    // "Please check": flagged fields and notices.
    static let caution = color(.caution)
    static let cautionIcon = color(.cautionIcon)
    static let cautionFill = color(.cautionFill)

    // Health notices.
    static let health = color(.health)
    static let healthIcon = color(.healthIcon)
    static let healthFill = color(.healthFill)

    // Overnight work.
    static let info = color(.info)
    static let infoIcon = color(.infoIcon)

    // A highlighted line in a transcript.
    static let highlight = color(.highlight)

    // The counts on section titles.
    static let badgeFill = color(.badgeFill)
    static let badgeLabel = color(.badgeLabel)

    // The red record circle, and the word under it.
    static let record = color(.record)
    static let recordText = color(.recordText)

    // Swipe action.
    static let swipe = color(.swipe)
    static let swipeLabel = color(.swipeLabel)

    // The map's branches (fills, lines and points; text on the map stays Theme.text on a card).
    static let mapPeople = color(.mapPeople)
    static let mapTopics = color(.mapTopics)
    static let mapPromises = color(.mapPromises)
    static let mapDecisions = color(.mapDecisions)
    static let mapQuestions = color(.mapQuestions)

    /// Links and controls.
    static let accent = link
    /// A switch that is on.
    static let toggleOn = positiveIcon

    /// A colour in one fixed mode.
    static func color(_ token: ThemeToken, _ mode: ThemeMode) -> Color { Color(uiColor: uiColor(ThemePalette.hex(token, mode))) }

    /// A placeholder in a text field. The system's own is too faint for elderly readers.
    static func prompt(_ key: LocalizedStringKey) -> Text { Text(key).foregroundStyle(secondary) }

    private static func color(_ token: ThemeToken) -> Color {
        Color(uiColor: UIColor { uiColor(ThemePalette.hex(token, $0.userInterfaceStyle == .dark ? .dark : .white)) })
    }

    static func uiColor(_ token: ThemeToken) -> UIColor {
        UIColor { uiColor(ThemePalette.hex(token, $0.userInterfaceStyle == .dark ? .dark : .white)) }
    }

    private static func uiColor(_ hex: UInt32) -> UIColor {
        UIColor(red: CGFloat((hex >> 16) & 0xFF) / 255, green: CGFloat((hex >> 8) & 0xFF) / 255, blue: CGFloat(hex & 0xFF) / 255, alpha: 1)
    }
}
