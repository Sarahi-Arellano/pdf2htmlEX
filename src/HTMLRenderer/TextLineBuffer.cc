/*
 * TextLineBuffer.cc
 *
 * Generate and optimized HTML for one line
 *
 * Copyright (C) 2012,2013 Lu Wang <coolwanglu@gmail.com>
 */

#include <vector>
#include <cmath>
#include <algorithm>

#include "HTMLRenderer.h"
#include "TextLineBuffer.h"
#include "util/namespace.h"
#include "util/unicode.h"
#include "util/math.h"
#include "util/css_const.h"
#include "util/encoding.h"

namespace pdf2htmlEX {

using std::min;
using std::max;
using std::vector;
using std::ostream;
using std::cerr;
using std::endl;
using std::find;
using std::abs;

void HTMLRenderer::TextLineBuffer::set_pos(GfxState * state)
{
    state->transform(state->getCurX(), state->getCurY(), &x, &y);
    tm_id = renderer->transform_matrix_manager.get_id();
}

void HTMLRenderer::TextLineBuffer::append_unicodes(const Unicode * u, int l)
{
    text.insert(text.end(), u, u+l);
}

void HTMLRenderer::TextLineBuffer::append_offset(double width)
{
    if((!offsets.empty()) && (offsets.back().start_idx == text.size()))
        offsets.back().width += width;
    else
        offsets.push_back(Offset({text.size(), width}));
}

void HTMLRenderer::TextLineBuffer::append_state(void)
{
    if(states.empty() || (states.back().start_idx != text.size()))
    {
        states.resize(states.size() + 1);
        states.back().start_idx = text.size();
        states.back().hash_umask = 0;
    }

    set_state(states.back());
}

void HTMLRenderer::TextLineBuffer::flush(void)
{
    /*
     * Each Line is an independent absolute positioned block
     * so even we have a few states or offsets, we may omit them
     */
    if(text.empty()) return;

    if(states.empty() || (states[0].start_idx != 0))
    {
        cerr << "Warning: text without a style! Must be a bug in pdf2htmlEX" << endl;
        return;
    }

    optimize();

    double max_ascent = 0;
    for(auto iter = states.begin(); iter != states.end(); ++iter)
    {
        const auto & s = *iter;
        max_ascent = max<double>(max_ascent, s.font_info->ascent * s.draw_font_size);
    }

    // append a dummy state for convenience
    states.resize(states.size() + 1);
    states.back().start_idx = text.size();
    
    for(auto iter = states.begin(); iter != states.end(); ++iter)
        iter->hash();

    // append a dummy offset for convenience
    offsets.push_back(Offset({text.size(), 0}));

    ostream & out = renderer->f_pages.fs;
    long long hid = renderer->height_manager.install(max_ascent);
    long long lid = renderer->left_manager  .install(x);
    long long bid = renderer->bottom_manager.install(y);

    out << "<div class=\"" << CSS::LINE_CN
        << " "             << CSS::TRANSFORM_MATRIX_CN << tm_id 
        << " "             << CSS::LEFT_CN             << lid
        << " "             << CSS::HEIGHT_CN           << hid
        << " "             << CSS::BOTTOM_CN           << bid
        << "\">";

    auto cur_state_iter = states.begin();
    auto cur_offset_iter = offsets.begin();

    //accumulated horizontal offset;
    double dx = 0;

    stack.clear();
    stack.push_back(nullptr);

    // whenever a negative offset appears, we should not pop out that <span>
    // otherwise the effect of negative margin-left would disappear
    size_t last_text_pos_with_negative_offset = 0;

    size_t cur_text_idx = 0;
    while(cur_text_idx < text.size())
    {
        // TODO: a new state may consume an offset with proper 'margin-left'
        if(cur_text_idx >= cur_state_iter->start_idx)
        {
            // greedy
            int best_cost = State::ID_COUNT;
            
            // we have a nullptr at the beginning, so no need to check for rend
            for(auto iter = stack.rbegin(); *iter; ++iter)
            {
                int cost = cur_state_iter->diff(**iter);
                if(cost < best_cost)
                {
                    while(stack.back() != *iter)
                    {
                        stack.back()->end(out);
                        stack.pop_back();
                    }
                    best_cost = cost;

                    if(best_cost == 0)
                        break;
                }

                // cannot go further
                if((*iter)->start_idx <= last_text_pos_with_negative_offset)
                    break;
            }
            cur_state_iter->begin(out, stack.back());
            stack.push_back(&*cur_state_iter);

            ++ cur_state_iter;
        }

        if(cur_text_idx >= cur_offset_iter->start_idx)
        {
            double target = cur_offset_iter->width + dx;
            double actual_offset = 0;

            if(abs(target) <= renderer->param->h_eps)
            {
                actual_offset = 0;
            }
            else
            {
                bool done = false;
                auto cur_state = stack.back();
                if(!(cur_state->hash_umask & State::umask_by_id(State::WORD_SPACE_ID)))
                {
                    double space_off = cur_state->single_space_offset();
                    if(abs(target - space_off) <= renderer->param->h_eps)
                    {
                        Unicode u = ' ';
                        outputUnicodes(out, &u, 1);
                        actual_offset = space_off;
                        done = true;
                    }
                }

                if(!done)
                {
                    long long wid = renderer->whitespace_manager.install(target, &actual_offset);

                    if(!equal(actual_offset, 0))
                    {
                        if(is_positive(-actual_offset))
                            last_text_pos_with_negative_offset = cur_text_idx;

                        double threshold = cur_state->em_size() * (renderer->param->space_threshold);

                        out << "<span class=\"" << CSS::WHITESPACE_CN
                            << ' ' << CSS::WHITESPACE_CN << wid << "\">" << (target > (threshold - EPS) ? " " : "") << "</span>";
                    }
                }
            }
            dx = target - actual_offset;
            ++ cur_offset_iter;
        }

        size_t next_text_idx = min<size_t>(cur_state_iter->start_idx, cur_offset_iter->start_idx);

        outputUnicodes(out, (&text.front()) + cur_text_idx, next_text_idx - cur_text_idx);
        cur_text_idx = next_text_idx;
    }

    // we have a nullptr in the bottom
    while(stack.back())
    {
        stack.back()->end(out);
        stack.pop_back();
    }

    out << "</div>";

    states.clear();
    offsets.clear();
    text.clear();
}

void HTMLRenderer::TextLineBuffer::set_state (State & state)
{
    state.ids[State::FONT_ID] = renderer->cur_font_info->id;
    state.ids[State::FONT_SIZE_ID] = renderer->font_size_manager.get_id();
    state.ids[State::FILL_COLOR_ID] = renderer->fill_color_manager.get_id();
    state.ids[State::STROKE_COLOR_ID] = renderer->stroke_color_manager.get_id();
    state.ids[State::LETTER_SPACE_ID] = renderer->letter_space_manager.get_id();
    state.ids[State::WORD_SPACE_ID] = renderer->word_space_manager.get_id();
    state.ids[State::RISE_ID] = renderer->rise_manager.get_id();

    state.font_info = renderer->cur_font_info;
    state.draw_font_size = renderer->font_size_manager.get_actual_value();
    state.letter_space = renderer->letter_space_manager.get_actual_value();
    state.word_space = renderer->word_space_manager.get_actual_value();
}

void HTMLRenderer::TextLineBuffer::optimize(void)
{
    assert(!states.empty());

    auto offset_iter = offsets.begin();
    std::map<double, int> width_map;

    // set proper hash_umask
    long long word_space_umask = State::umask_by_id(State::WORD_SPACE_ID);
    for(auto state_iter2 = states.begin(), state_iter1 = state_iter2++; 
            state_iter1 != states.end(); 
            ++state_iter1, ++state_iter2)
    {
        size_t text_idx1 = state_iter1->start_idx;
        size_t text_idx2 = (state_iter2 == states.end()) ? text.size() : state_iter2->start_idx;

        // get the text segment covered by current state (*state_iter1)
        auto text_iter1 = text.begin() + text_idx1;
        auto text_iter2 = text.begin() + text_idx2;

        // In some PDF files all spaces are converted into positionig shift
        // We may try to change (some of) them to ' ' and adjust word_space accordingly
        // This can also be applied when param->space_as_offset is set
        // for now, we cosider only the no-space scenario
        if(find(text_iter1, text_iter2, ' ') != text_iter2)
            continue;

        // if there is not any space, we may change the value of word_space arbitrarily
        // collect widths
        width_map.clear();

        while((offset_iter != offsets.end()) && (offset_iter->start_idx < text_idx1))
            ++ offset_iter;

        double threshold = (state_iter1->em_size()) * (renderer->param->space_threshold);
        for(; (offset_iter != offsets.end()) && (offset_iter->start_idx < text_idx2); ++offset_iter)
        {
            double target = offset_iter->width;
            // we don't want to add spaces for tiny gaps, or even negative shifts
            if(target < threshold - EPS)
                continue;

            auto iter = width_map.lower_bound(target-EPS);
            if((iter != width_map.end()) && (abs(iter->first - target) <= EPS))
            {
                ++ iter->second;
            }
            else
            {
                width_map.insert(iter, std::make_pair(target, 1));
            }
        }
        if(width_map.empty())
        {
            // if there is no offset at all
            // we just free word_space
            state_iter1->hash_umask |= word_space_umask;
            continue;
        }

        // set word_space for the most frequently used offset
        double most_used_width = 0;
        int max_count = 0;
        for(auto iter = width_map.begin(); iter != width_map.end(); ++iter)
        {
            if(iter->second > max_count)
            {
                max_count = iter->second;
                most_used_width = iter->first;
            }
        }
        
        state_iter1->word_space = 0;
        double new_word_space = most_used_width - state_iter1->single_space_offset();
        // install new word_space
        state_iter1->ids[State::WORD_SPACE_ID] = renderer->word_space_manager.install(new_word_space, &(state_iter1->word_space));
        // mark that the word_space is not free
        state_iter1->hash_umask &= (~word_space_umask);
    } 
}

// this state will be converted to a child node of the node of prev_state
// dump the difference between previous state
// also clone corresponding states
void HTMLRenderer::TextLineBuffer::State::begin (ostream & out, const State * prev_state)
{
    long long cur_mask = 0xff;
    bool first = true;
    for(int i = 0; i < ID_COUNT; ++i, cur_mask<<=8)
    {
        if(hash_umask & cur_mask) // we don't care about this ID
        {
            if (prev_state && (!(prev_state->hash_umask & cur_mask))) // if prev_state have it set
            {
                // we have to inherit it
                ids[i] = prev_state->ids[i]; 
                hash_umask &= (~cur_mask);
                //copy the corresponding value
                //TODO: this is so ugly
                switch(i)
                {
                case FONT_SIZE_ID:
                    draw_font_size = prev_state->draw_font_size;
                    break;
                case LETTER_SPACE_ID:
                    letter_space = prev_state->letter_space;
                    break;
                case WORD_SPACE_ID:
                    word_space = prev_state->word_space;
                    break;
                default:
                    break;
                }
            }
            //anyway we don't have to output it
            continue;
        }

        // now we care about the ID
        if(prev_state && (!(prev_state->hash_umask & cur_mask)) && (prev_state->ids[i] == ids[i]))
            continue;

        if(first)
        { 
            out << "<span class=\"";
            first = false;
        }
        else
        {
            out << ' ';
        }

        // out should have hex set
        out << css_class_names[i];
        if (ids[i] == -1)
            out << CSS::INVALID_ID;
        else
            out << ids[i];
    }

    if(first) // we actually just inherit the whole prev_state
    {
        need_close = false;
    }
    else
    {
        out << "\">";
        need_close = true;
    }
}

void HTMLRenderer::TextLineBuffer::State::end(ostream & out) const
{
    if(need_close)
        out << "</span>";
}

void HTMLRenderer::TextLineBuffer::State::hash(void)
{
    hash_value = 0;
    for(int i = 0; i < ID_COUNT; ++i)
    {
        hash_value = (hash_value << 8) | (ids[i] & 0xff);
    }
}

int HTMLRenderer::TextLineBuffer::State::diff(const State & s) const
{
    /*
     * A quick check based on hash_value
     * it could be wrong when there are more then 256 classes, 
     * in which case the output may not be optimal, but still 'correct' in terms of HTML
     */
    long long common_mask = ~(hash_umask | s.hash_umask);
    if((hash_value & common_mask) == (s.hash_value & common_mask)) return 0;

    long long cur_mask = 0xff;
    int d = 0;
    for(int i = 0; i < ID_COUNT; ++i)
    {
        if((common_mask & cur_mask) && (ids[i] != s.ids[i]))
            ++ d;
        cur_mask <<= 8;
    }
    return d;
}

double HTMLRenderer::TextLineBuffer::State::single_space_offset(void) const
{
    return word_space + letter_space + font_info->space_width * draw_font_size;
}

double HTMLRenderer::TextLineBuffer::State::em_size(void) const
{
    return draw_font_size * (font_info->ascent - font_info->descent);
}

long long HTMLRenderer::TextLineBuffer::State::umask_by_id(int id)
{
    return (((long long)0xff) << (8*id));
}

// the order should be the same as in the enum
const char * const HTMLRenderer::TextLineBuffer::State::css_class_names [] = {
    CSS::FONT_FAMILY_CN,
    CSS::FONT_SIZE_CN,
    CSS::FILL_COLOR_CN,
    CSS::STROKE_COLOR_CN,
    CSS::LETTER_SPACE_CN,
    CSS::WORD_SPACE_CN,
    CSS::RISE_CN
};

} //namespace pdf2htmlEX
