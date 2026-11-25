#pragma once
// About a thousand frequent English words (lowercase), for telling a sentence's capitalised first word
// ("Arrange chairs ...") from a name ("Kiran will pay"). Words that are also common first names
// (mark, bill, grace, rose, hope, joy, frank, ray, jack, sunny, ...) are left out on purpose:
// a left-out word is checked as a name, which can only drop a memory, never let a wrong one through.
#include <set>
#include <sstream>
#include <string>

namespace sve {

inline bool common_english_word(const std::string &lower) {
    static const std::set<std::string> kWords = [] {
        static const char *kList =
            // function words
            "a about above across after afterwards again against ago all almost alone along already also although "
            "always am among an and another any anybody anyone anything anyway anywhere are around as at away back "
            "be became because become been before behind being below beside besides between beyond both but by "
            "can cannot could did do does doing done down during each either else elsewhere enough even ever every "
            "everybody everyone everything everywhere except few for from further had has have having he her here "
            "hers herself him himself his how however i if in inside instead into is it its itself just least less "
            "let lot lots many may maybe me might mine more most mostly much must my myself near nearly neither "
            "never nevertheless next no nobody none noone nor not nothing now nowhere of off often on once one only "
            "onto or other others otherwise ought our ours ourselves out outside over own per perhaps please quite "
            "rather really same several shall she should since so some somebody someone something sometimes "
            "somewhere soon still such than that the their theirs them themselves then there therefore these they "
            "this those though through throughout thus till to together too toward towards under unless until up "
            "upon us very via was we well were what whatever when whenever where wherever whether which while who "
            "whoever whole whom whose why will with within without would yes yet you your yours yourself "
            "yourselves okay ok hi hello thanks thank sorry yeah oh "
            // contractions
            "don't can't won't didn't doesn't isn't aren't wasn't weren't let's i'm i'll i'd i've we'll we're we've "
            "we'd it's that's there's what's who's he's she's they're they'll they've you're you'll you've "
            "shouldn't couldn't wouldn't haven't hasn't hadn't mustn't needn't here's where's how's "
            // time words
            "today tomorrow yesterday tonight morning afternoon evening night day days week weeks month months year "
            "years weekend hour hours minute minutes time times early late later earlier soon daily weekly monthly "
            "yearly annual first second third last final now then before after during meanwhile "
            // verbs
            "accept accepted add added admit advise afford agree agreed allow allowed answer answered apply applied "
            "appoint arrange arranged arrive arrived ask asked attend attended avoid avoided bake be bear become "
            "begin began believe belong book booked borrow borrowed break bring brought build built buy bought "
            "call called came cancel cancelled care carry carried catch caught celebrate change changed charge "
            "charged check checked choose chose clean cleaned clear cleared close closed collect collected come "
            "compare complete completed confirm confirmed connect consider contact contacted continue cook cooked "
            "copy correct cost could count cover create created cross cut deal decide decided deliver delivered "
            "depend describe design did discuss discussed do does done drink drive drop dropped eat email emailed "
            "end ended enjoy enter explain explained fall feel felt fill filled find found finish finished fix "
            "fixed follow followed forget forgot forgive form found get gets getting give given gave go goes going "
            "gone got grow guess handle hang happen happened hate have hear heard help helped hold hurry "
            "imagine improve include increase inform informed install invite invited join joined keep kept know "
            "knew known learn learned leave left lend let lie like liked listen live lived look looked lose lost "
            "love make made manage mean meant meet met mention mentioned mind miss missed move moved need needed "
            "note noted notice offer offered open opened order ordered organise organize owe owed pack packed pass "
            "passed pay paid pick picked plan planned play played post prefer prepare prepared print promise "
            "promised protect prove provide pull push put raise reach read ready realise realize receive received "
            "recommend record recorded reduce refer register registered remember remind reminded remove renew "
            "rent repair repaired repeat replace replaced reply replied report reported request requested reserve "
            "reserved rest return returned review reviewed ring run said save saved say says see seen send sent "
            "set settle share shared shift show showed sign signed sit sleep solve sort speak spend spent start "
            "started stay stayed stop stopped study submit submitted suggest suggested supply support suppose "
            "take taken talk talked teach tell think thought throw told took train transfer travel tried try turn "
            "understand update updated upload use used visit visited wait waited wake walk want wanted wash watch "
            "wear went win wish wonder work worked worry write written wrote "
            // nouns
            "account address advance amount answer apartment appointment area arrangement article bag balance bank "
            "bills birthday board body book books box boy branch budget building bus business car card case "
            "cash chair chairs chance chapter child children city class client clothes code college company "
            "computer cost course customer data date deadline deal decision delivery department deposit detail "
            "details discussion document documents door draft dress driver due electricity email exam example "
            "family fee fees file files flat floor food form friend friends function fund gas gift girl goal group "
            "guest guests hall home homework house idea ideas information insurance interest internet invoice item "
            "items job key kids kitchen land laptop lecture lesson letter level light list loan lunch dinner "
            "breakfast machine main market match meeting meetings member members message money name number office "
            "order owner page paper parcel parent parents part party password payment people person phone photo "
            "plan plans point points policy price problem process product project projects question questions "
            "receipt rent reply report result results room rule salary school seat service session shop side "
            "sister brother mother father son daughter uncle aunt cousin wife husband teacher student students "
            "boss team ticket tickets topic train trip type university vehicle water website work world "
            // adjectives and adverbs
            "able actual actually available bad basic best better big busy careful certain cheap clean clear "
            "close cold common correct current dear different difficult due easy empty entire exact exactly extra "
            "fast fine free fresh full general good great half happy hard heavy high important interesting kind "
            "large late little local long low main major new nice normal old open other important past possible "
            "pretty previous private proper quick quickly ready real recent regular right safe short simple "
            "small special strong sure true important usual usually whole wrong young also definitely probably "
            "maybe surely tentatively finally firstly secondly next "
            // question and discourse starters
            "remember note reminder reminders decision decided promise question idea fact person todo to-do";
        std::set<std::string> s;
        std::istringstream in(kList);
        for (std::string w; in >> w;) s.insert(w);
        return s;
    }();
    return kWords.count(lower) > 0;
}

}  // namespace sve
