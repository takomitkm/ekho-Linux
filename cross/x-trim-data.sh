#!/bin/bash
# ASCII only. Drop the runtime-dead parts of an assembled kit's data, so the
# shipped kit carries Mandarin + English and nothing else.
#
# usage: x-trim-data.sh [-n] EKHO_DATA_DIR [ESPEAK_DATA_DIR]
#   -n   report what would go, delete nothing
#
# This trims a COPY. The kit script copies ekho-data out of the install tree
# and espeak-ng-data out of the espeak kit before calling this, so the sources
# of truth are untouched and a rebuild always starts from the full data.
#
# Why each keep is a keep (checked in the source, not guessed):
#
#   zh.dict           loadMandarin() reads it when present (ekho_dict.cpp:215)
#   pinyin.voice      Mandarin audio, loadEkhoVoiceFile()
#   pinyin.index      its offsets; setVoice() takes the .index path when the
#                     index exists (ekho_dict.cpp:557)
#   alphabet/         26 letter wavs read by addSpecialSymbols() on EVERY
#                     setLanguage() call (ekho_dict.cpp:332)
#
# Why each drop is a drop:
#
#   pinyin/           2060 loose single-syllable wavs. Every name has exactly
#                     one tone digit, and findMatchedPinyin() matches with
#                     count = size-1 = 0, so no name can ever match: this is
#                     makeVoiceFile() source material, read only when the
#                     .index is missing (to regenerate it). The kit ships the
#                     .index. RECORDING synthesis reads a loose file only for
#                     multi-syllable names, of which there are none.
#   *.orig            the archived GSM originals; loadEkhoVoiceFile() reads
#                     exactly path+".index"/path+".voice", never .orig
#   zh_list, zh_listx, zh_patch, zhy_list
#                     loadEspeakDict() fallbacks, taken only when the .dict
#                     is ABSENT; the kit ships zh.dict
#   bo_list, ka_list, ko_list, ngangien_list, toisanese_list,
#   hakka/, hangul/, ngangien/, toisanese/, piper/, friso-dict/
#                     languages this kit does not ship (the tree carries the
#                     Mandarin voice data alone since 2026-11-02), plus friso's
#                     lexicon (its segmentation code is commented out in
#                     word.cpp and it is not in Makefile.am, so friso-dict is
#                     never opened)
#   espeak's other ~95 *_dict and mbrola_ph/
#                     English is the only language that reaches espeak; the
#                     board has no mbrola. phondata/phonindex/phontab and
#                     voices/ (+lang/) stay: they are the shared phoneme
#                     tables and the voice/variant definitions (en-us+f3).
#
# /voices shrinks to Mandarin/English on purpose: the server probes
# each voice with setVoice() and loadEspeakDict() fails cleanly on a missing
# file (ekho_dict.cpp:932), so the probe skips what is not shipped instead of
# pretending.

DRY=""
if [ "${1:-}" = "-n" ]; then DRY=1; shift; fi
ED=${1:-}
[ -n "$ED" ] || { echo "usage: x-trim-data.sh [-n] EKHO_DATA_DIR [ESPEAK_DATA_DIR]" >&2; exit 2; }
[ -d "$ED" ] || { echo "trim: no such dir: $ED" >&2; exit 2; }
ES=${2:-}
[ -n "$ES" ] && [ ! -d "$ES" ] && { echo "trim: no such dir: $ES" >&2; exit 2; }

kb() { du -sk "$1" 2>/dev/null | cut -f1; }

ED_BEFORE=$(kb "$ED")

echo "=== ekho-data: $ED_BEFORE KB in $(find "$ED" -type f | wc -l) files ==="

# Top level of ekho-data: keep exactly the names above, whatever else
# is there. Iterating the directory (not a fixed drop list) means a file
# someone adds later is dropped too, and shows up in this report.
for p in "$ED"/* "$ED"/.[!.]*; do
    [ -e "$p" ] || continue
    n=$(basename "$p")
    case $n in
        zh.dict|pinyin.voice|pinyin.index|alphabet)
            printf '  keep %-18s %6s KB\n' "$n" "$(kb "$p")" ;;
        *)
            s=$(kb "$p"); f=$(find "$p" -type f 2>/dev/null | wc -l)
            if [ -n "$DRY" ]; then
                printf '  DROP %-18s %6s KB  (%s files)\n' "$n" "$s" "$f"
            else
                rm -rf "$p"
                printf '  drop %-18s %6s KB  (%s files)\n' "$n" "$s" "$f"
            fi ;;
    esac
done

ED_AFTER=$(kb "$ED")
echo "  ekho-data: $ED_BEFORE KB -> $ED_AFTER KB"

if [ -n "$ES" ]; then
    ES_BEFORE=$(kb "$ES")
    echo
    echo "=== espeak-ng-data: $ES_BEFORE KB in $(find "$ES" -type f | wc -l) files ==="
    # Keep the shared phoneme tables, the voice/variant definitions and the
    # English dictionary; drop every other language's dictionary.
    for p in "$ES"/* "$ES"/.[!.]*; do
        [ -e "$p" ] || continue
        n=$(basename "$p")
        case $n in
            en_dict|phondata|phondata-manifest|phonindex|phontab|intonations|voices|lang)
                printf '  keep %-18s %6s KB\n' "$n" "$(kb "$p")" ;;
            *)
                s=$(kb "$p"); f=$(find "$p" -type f 2>/dev/null | wc -l)
                if [ -n "$DRY" ]; then
                    printf '  DROP %-18s %6s KB  (%s files)\n' "$n" "$s" "$f"
                else
                    rm -rf "$p"
                    printf '  drop %-18s %6s KB  (%s files)\n' "$n" "$s" "$f"
                fi ;;
        esac
    done
    ES_AFTER=$(kb "$ES")
    echo "  espeak-ng-data: $ES_BEFORE KB -> $ES_AFTER KB"
fi

echo
echo "trim: saved $((ED_BEFORE - ED_AFTER)) KB$( [ -n "$ES" ] && echo " + $((ES_BEFORE - ES_AFTER)) KB" )"
if [ -n "$DRY" ]; then echo "trim: dry run, nothing deleted"; fi
