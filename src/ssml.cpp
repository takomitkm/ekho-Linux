/***************************************************************************
 * Copyright (C) 2008-2020 by Cameron Wong                                 *
 * name in passport: HUANG GUANNENG                                        *
 * email: hgneng at gmail.com                                              *
 * website: http://www.eguidedog.net                                       *
 *                                                                         *
 * This program is free software; you can redistribute it and/or           *
 * modify it under the terms of the GNU General Public License             *
 * as published by the Free Software Foundation; either version 2          *
 * of the License, or any later version.                                   *
 *                                                                         *
 * This program is distributed in the hope that it will be useful,         *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 * GNU General Public License for more details.                            *
 *                                                                         *
 * You should have received a copy of the GNU General Public License       *
 * along with this program; if not, write to the Free Software             *
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,              *
 * MA  02110-1301, USA.                                                    *
 **************************************************************************/

#include <string>
#include <iostream>
#include "ssml.h"
#include "utf8.h"
using namespace std;
using namespace ekho;

/**
 * Filter tags:
 * Ex: <speak><mark name="0:8"/>屏幕阅读器启用。</speak>
 * <speak><mark name="0:1"/>左 <mark name="2:7"/>Shift </speak>
 *
 * Real-world SSML carries attributes (<speak version="1.0" xml:lang="zh">)
 * and may arrive as several wrappers in a row, so the tags are stripped
 * positionally instead of being paired: the closing needle used to be built
 * from the whole opening tag text (</speak version="1.0">), which never
 * matched, and the pairing required the closer to sit at the very end -- tags
 * then reached the engine and were read aloud, and two consecutive wrappers
 * left debris between them.
 */
string Ssml::stripTags(const string& text) {
  string out = text;

  // <mark .../> self-closing markers.
  size_t pos;
  while ((pos = out.find("<mark")) != string::npos) {
    size_t end = out.find("/>", pos);
    if (end == string::npos) break;  // malformed: leave the rest untouched
    out.erase(pos, end + 2 - pos);
  }

  // <speak ...> openers (attributes allowed) and </speak> closers.
  while ((pos = out.find("<speak")) != string::npos) {
    size_t gt = out.find('>', pos);
    if (gt == string::npos) break;
    out.erase(pos, gt + 1 - pos);
  }
  while ((pos = out.find("</speak>")) != string::npos) {
    out.erase(pos, 8);
  }

  return out;
}

// sample: <audio src=\"%s%s\">%s</audio>
bool Ssml::isAudio(const string& text) {
	return text.find("<audio src=") == 0;
}

string Ssml::getAudioPath(const string& text) {
	int start = text.find('"');
	int end = text.rfind('"');
	if (start > 1 && end > 2 && end > start) {
		return text.substr(start + 1, end - start - 1);
	}

	return "";
}

// 过滤文本前后空白字符，及中文字间空白字符
void Ssml::filterSpaces(string &text) {
  bool changed = false;
  string text2;

  size_t first = text.find_first_not_of(' ');
  if (first == std::string::npos) {
    // If the string is all spaces
    text = "";
    return;
  }

  // 去掉前后空格
  size_t last = text.find_last_not_of(' ');
  if (text.length() != last - first + 1) {
    text = text.substr(first, last - first + 1);
  }

  bool in_chinese_context = true;

  int c;
  string::iterator it = text.begin();
  string::iterator it2 = text.begin();
  string::iterator end = text.end();

  while (it != end) {
    it2 = it;
#if 1
    try {
      c = utf8::next(it, end);
    } catch (utf8::not_enough_room &) {
      text = text2;
      return;
    } catch (utf8::invalid_utf8 &) {
      cerr << "filterSpaces: Invalid UTF8 encoding" << endl;
      text = text2;
      return;
    }
#endif

    if (in_chinese_context && (c == 32 || c == 12288)) {
      // 去掉中文字间空白字符
      changed = true;
    } else {
      while (it2 != it) {
        text2.push_back(*it2);
        it2++;
      }
      in_chinese_context = (c > 128);
    }

    while (it2 != it) it2++;
  }

  if (changed) {
    text = text2;
  }
}