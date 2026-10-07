#include "stdafx.h"
#include "libretro_loader.h"

#include "Utilities/File.h"

#include <stb_image.h>
#include <stb_truetype.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>

namespace libretro_loader
{
	namespace
	{
		// ---- Palette ------------------------------------------------------------
		// A deep violet night, with violet to orchid for progress.

		struct colour
		{
			float r, g, b;
		};

		constexpr colour hex(u32 value)
		{
			return {((value >> 16) & 0xff) / 255.f, ((value >> 8) & 0xff) / 255.f, (value & 0xff) / 255.f};
		}

		constexpr colour sky_top = hex(0x1d1040);
		constexpr colour sky_bottom = hex(0x07040e);
		constexpr colour glow_violet = hex(0x6d28d9);
		constexpr colour glow_orchid = hex(0xa21caf);
		constexpr colour fill_start = hex(0x7c3aed);
		constexpr colour fill_end = hex(0xd946ef);
		constexpr colour error_start = hex(0xe11d48);
		constexpr colour error_end = hex(0xfb7185);
		constexpr colour heading_colour = hex(0xc4b5fd);
		constexpr colour title_colour = hex(0xf5f3ff);
		constexpr colour muted_colour = hex(0xa89cc8);
		constexpr colour faint_colour = hex(0x6f6194);
		constexpr colour white = hex(0xffffff);
		constexpr colour black = hex(0x000000);

		colour mix(colour a, colour b, float t)
		{
			return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
		}

		// B8G8R8A8, as RPCS3's own frames.
		u32 pack(float r, float g, float b)
		{
			const auto channel = [](float v) { return static_cast<u32>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
			return 0xff000000u | channel(r) << 16 | channel(g) << 8 | channel(b);
		}

		colour unpack(u32 pixel)
		{
			return {((pixel >> 16) & 0xff) / 255.f, ((pixel >> 8) & 0xff) / 255.f, (pixel & 0xff) / 255.f};
		}

		struct canvas
		{
			u32 width = 0;
			u32 height = 0;
			std::vector<u32> pixels;

			void blend(int x, int y, colour c, float alpha)
			{
				if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height) || alpha <= 0.f)
					return;
				u32& pixel = pixels[static_cast<usz>(y) * width + x];
				const colour under = unpack(pixel);
				const float a = std::min(alpha, 1.f);
				pixel = pack(under.r + (c.r - under.r) * a, under.g + (c.g - under.g) * a, under.b + (c.b - under.b) * a);
			}
		};

		float smoothstep(float edge0, float edge1, float x)
		{
			const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.f, 1.f);
			return t * t * (3.f - 2.f * t);
		}

		// The signed distance from a rounded rectangle's edge: negative inside.
		float rounded_distance(float px, float py, float x, float y, float w, float h, float radius)
		{
			const float half_w = w / 2, half_h = h / 2;
			const float qx = std::abs(px - (x + half_w)) - (half_w - radius);
			const float qy = std::abs(py - (y + half_h)) - (half_h - radius);
			const float outside = std::hypot(std::max(qx, 0.f), std::max(qy, 0.f));
			const float inside = std::min(std::max(qx, qy), 0.f);
			return outside + inside - radius;
		}

		// Shades every pixel near a rounded rectangle: `shade(x, y, distance)`
		// blends the pixel itself. `reach` is how far outside the edge it goes.
		template <typename F>
		void shade_rounded(canvas& c, float x, float y, float w, float h, float radius, float reach, F&& shade)
		{
			const int x0 = std::max(0, static_cast<int>(std::floor(x - reach)));
			const int y0 = std::max(0, static_cast<int>(std::floor(y - reach)));
			const int x1 = std::min(static_cast<int>(c.width), static_cast<int>(std::ceil(x + w + reach)));
			const int y1 = std::min(static_cast<int>(c.height), static_cast<int>(std::ceil(y + h + reach)));
			for (int py = y0; py < y1; py++)
				for (int px = x0; px < x1; px++)
					shade(px, py, rounded_distance(px + 0.5f, py + 0.5f, x, y, w, h, radius));
		}

		// A soft glow or shadow around a rounded rectangle.
		void glow(canvas& c, float x, float y, float w, float h, float radius, float sigma, colour tint, float strength)
		{
			shade_rounded(c, x, y, w, h, radius, sigma * 3, [&](int px, int py, float d)
			{
				const float outside = std::max(d, 0.f);
				c.blend(px, py, tint, strength * std::exp(-(outside * outside) / (2 * sigma * sigma)));
			});
		}

		// ---- Images -------------------------------------------------------------

		struct image
		{
			int width = 0;
			int height = 0;
			std::vector<u8> rgba;

			bool load(const std::string& path)
			{
				rgba.clear();
				std::vector<u8> bytes;
				if (fs::file file{path}; !file || !file.read(bytes, file.size()))
					return false;
				int channels = 0;
				stbi_uc* data = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 4);
				if (!data)
					return false;
				rgba.assign(data, data + static_cast<usz>(width) * height * 4);
				stbi_image_free(data);
				return true;
			}

			// Bilinear, with coordinates in pixels.
			std::array<float, 4> sample(float x, float y) const
			{
				x = std::clamp(x - 0.5f, 0.f, width - 1.f);
				y = std::clamp(y - 0.5f, 0.f, height - 1.f);
				const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
				const int x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
				const float fx = x - x0, fy = y - y0;
				std::array<float, 4> out{};
				for (int i = 0; i < 4; i++)
				{
					const auto at = [&](int px, int py) { return rgba[(static_cast<usz>(py) * width + px) * 4 + i] / 255.f; };
					const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
					const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
					out[i] = top + (bottom - top) * fy;
				}
				return out;
			}
		};

		// The game's background art, small and blurred, to tint the sky with.
		image blurred(const image& art, int width, int height)
		{
			image out;
			out.width = width;
			out.height = height;
			out.rgba.resize(static_cast<usz>(width) * height * 4);
			// Cover: the art's middle at the screen's aspect.
			const float scale = std::max(static_cast<float>(width) / art.width, static_cast<float>(height) / art.height);
			const float left = (art.width - width / scale) / 2, top = (art.height - height / scale) / 2;
			for (int y = 0; y < height; y++)
				for (int x = 0; x < width; x++)
				{
					const auto texel = art.sample(left + (x + 0.5f) / scale, top + (y + 0.5f) / scale);
					for (int i = 0; i < 4; i++)
						out.rgba[(static_cast<usz>(y) * width + x) * 4 + i] = static_cast<u8>(texel[i] * 255.f + 0.5f);
				}
			// Three box passes each way: close to a Gaussian.
			std::vector<u8> line;
			const auto pass = [&](bool across)
			{
				const int count = across ? height : width, length = across ? width : height, radius = 3;
				line.resize(static_cast<usz>(length) * 4);
				for (int n = 0; n < count; n++)
				{
					const auto at = [&](int i) -> u8* { return &out.rgba[((across ? static_cast<usz>(n) * width + i : static_cast<usz>(i) * width + n)) * 4]; };
					for (int i = 0; i < length; i++)
						std::memcpy(&line[static_cast<usz>(i) * 4], at(i), 4);
					for (int i = 0; i < length; i++)
						for (int ch = 0; ch < 3; ch++)
						{
							int sum = 0;
							for (int j = -radius; j <= radius; j++)
								sum += line[static_cast<usz>(std::clamp(i + j, 0, length - 1)) * 4 + ch];
							at(i)[ch] = static_cast<u8>(sum / (2 * radius + 1));
						}
				}
			};
			for (int i = 0; i < 3; i++)
			{
				pass(true);
				pass(false);
			}
			return out;
		}

		// ---- Fonts --------------------------------------------------------------

		struct font
		{
			std::vector<u8> data;
			stbtt_fontinfo info{};
			bool loaded = false;

			struct glyph
			{
				std::vector<u8> coverage;
				int width = 0, height = 0, left = 0, top = 0;
				float advance = 0;
			};
			std::map<std::pair<int, char32_t>, glyph> glyphs;

			bool load(const std::string& path)
			{
				fs::file file{path};
				if (!file || !file.read(data, file.size()) ||
					!stbtt_InitFont(&info, data.data(), stbtt_GetFontOffsetForIndex(data.data(), 0)))
					return false;
				glyphs.clear();
				return loaded = true;
			}

			float scale(float size) const { return stbtt_ScaleForPixelHeight(&info, size); }

			char32_t present(char32_t c) const { return stbtt_FindGlyphIndex(&info, static_cast<int>(c)) ? c : U'?'; }

			const glyph& get(float size, char32_t c)
			{
				auto [it, added] = glyphs.try_emplace({static_cast<int>(size * 4), c});
				if (added)
				{
					const float s = scale(size);
					int advance = 0, bearing = 0;
					stbtt_GetCodepointHMetrics(&info, static_cast<int>(c), &advance, &bearing);
					it->second.advance = advance * s;
					if (unsigned char* bitmap = stbtt_GetCodepointBitmap(&info, s, s, static_cast<int>(c), &it->second.width, &it->second.height, &it->second.left, &it->second.top))
					{
						it->second.coverage.assign(bitmap, bitmap + static_cast<usz>(it->second.width) * it->second.height);
						stbtt_FreeBitmap(bitmap, nullptr);
					}
				}
				return it->second;
			}

			float kerning(float size, char32_t a, char32_t b) const
			{
				return stbtt_GetCodepointKernAdvance(&info, static_cast<int>(a), static_cast<int>(b)) * scale(size);
			}
		};

		std::vector<std::string> font_dirs;
		font regular;
		font bold;
		std::chrono::steady_clock::time_point fonts_tried{};

		// Inter first, then the firmware's Rodin, which exists only once the
		// firmware is installed: tried again every second until found.
		void find_fonts()
		{
			if (regular.loaded && bold.loaded)
				return;
			const auto now = std::chrono::steady_clock::now();
			if (now - fonts_tried < std::chrono::seconds(1))
				return;
			fonts_tried = now;
			const auto find = [](font& f, std::initializer_list<const char*> names)
			{
				for (const std::string& dir : font_dirs)
					for (const char* name : names)
						if (!f.loaded && fs::is_file(dir + name))
							f.load(dir + name);
			};
			find(regular, {"Inter-Regular.ttf", "SCE-PS3-RD-R-LATIN.TTF"});
			find(bold, {"Inter-SemiBold.ttf", "SCE-PS3-RD-B-LATIN.TTF"});
		}

		std::u32string decode(std::string_view text)
		{
			std::u32string out;
			for (usz i = 0; i < text.size();)
			{
				const u8 lead = static_cast<u8>(text[i]);
				const usz length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xe ? 3 : (lead >> 3) == 0x1e ? 4 : 0;
				if (!length || i + length > text.size())
				{
					out += U'?';
					i++;
					continue;
				}
				char32_t c = length == 1 ? lead : lead & (0x7f >> length);
				for (usz j = 1; j < length; j++)
					c = (c << 6) | (static_cast<u8>(text[i + j]) & 0x3f);
				// A PS3 title's line breaks read as spaces here.
				out += (c == U'\n' || c == U'\r') ? U' ' : c;
				i += length;
			}
			return out;
		}

		float measure(font& f, float size, std::u32string_view text, float tracking)
		{
			float width = 0;
			for (usz i = 0; i < text.size(); i++)
			{
				const char32_t c = f.present(text[i]);
				width += f.get(size, c).advance + tracking;
				if (i + 1 < text.size())
					width += f.kerning(size, c, f.present(text[i + 1]));
			}
			return text.empty() ? 0 : width - tracking;
		}

		void draw_text(canvas& c, font& f, float size, std::u32string_view text, float x, float baseline, colour tint, float alpha = 1.f, float tracking = 0)
		{
			if (!f.loaded)
				return;
			for (usz i = 0; i < text.size(); i++)
			{
				const char32_t ch = f.present(text[i]);
				const font::glyph& g = f.get(size, ch);
				const int left = static_cast<int>(std::lround(x)) + g.left;
				const int top = static_cast<int>(std::lround(baseline)) + g.top;
				for (int gy = 0; gy < g.height; gy++)
					for (int gx = 0; gx < g.width; gx++)
						if (const u8 cover = g.coverage[static_cast<usz>(gy) * g.width + gx])
							c.blend(left + gx, top + gy, tint, alpha * cover / 255.f);
				x += g.advance + tracking;
				if (i + 1 < text.size())
					x += f.kerning(size, ch, f.present(text[i + 1]));
			}
		}

		// Text that fits `width`: smaller down to `smallest`, then cut short.
		std::u32string fit(font& f, float& size, float smallest, std::u32string text, float width)
		{
			if (!f.loaded)
				return text;
			while (size > smallest && measure(f, size, text, 0) > width)
				size = std::max(smallest, size * 0.94f);
			if (measure(f, size, text, 0) <= width)
				return text;
			while (!text.empty() && measure(f, size, text + U"…", 0) > width)
				text.pop_back();
			return text + U"…";
		}

		// ---- The screen ---------------------------------------------------------

		canvas frame;
		std::vector<u32> sky;  // the background alone
		u32 sky_width = 0;
		u32 sky_height = 0;
		std::string art_dir;   // the folder the sky's art was looked for in
		bool art_found = false;
		image icon;
		std::string icon_dir;
		u32 images_version = 0;  // changes with the sky or the icon
		std::vector<u32> layer;  // the sky with what stays put: icon, heading, title, footer
		std::string layer_key;
		std::chrono::steady_clock::time_point images_tried{};
		const auto started = std::chrono::steady_clock::now();

		void paint_sky(const image* art)
		{
			const u32 width = frame.width, height = frame.height;
			sky.resize(static_cast<usz>(width) * height);
			sky_width = width;
			sky_height = height;
			images_version++;
			image small;
			if (art)
				small = blurred(*art, 192, 108);
			// A 4x4 ordered dither, so the gradients do not band.
			static constexpr float bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
			for (u32 y = 0; y < height; y++)
				for (u32 x = 0; x < width; x++)
				{
					const float fx = (x + 0.5f) / width, fy = (y + 0.5f) / height;
					colour c = mix(sky_top, sky_bottom, smoothstep(0.f, 1.f, fy));
					const float dx = (fx - 0.5f) / 0.55f, dy = (fy - 0.66f) / 0.5f;
					const float violet = 0.40f * std::exp(-(dx * dx + dy * dy) * 2.2f);
					const float ox = (fx - 0.86f) / 0.35f, oy = (fy - 0.08f) / 0.45f;
					const float orchid = 0.20f * std::exp(-(ox * ox + oy * oy) * 2.0f);
					c = {c.r + glow_violet.r * violet + glow_orchid.r * orchid, c.g + glow_violet.g * violet + glow_orchid.g * orchid,
						c.b + glow_violet.b * violet + glow_orchid.b * orchid};
					if (art)
					{
						// The art, dimmed and pushed towards violet, under the glow.
						const auto texel = small.sample(fx * small.width, fy * small.height);
						const float light = 0.34f;
						c = {c.r * 0.7f + texel[0] * light * 0.85f, c.g * 0.7f + texel[1] * light * 0.7f, c.b * 0.7f + texel[2] * light * 1.1f};
					}
					const float vx = fx - 0.5f, vy = fy - 0.5f;
					const float vignette = 1.f - 0.55f * smoothstep(0.2f, 0.75f, std::sqrt(vx * vx + vy * vy));
					const float noise = (bayer[y & 3][x & 3] / 16.f - 0.5f) / 255.f;
					sky[static_cast<usz>(y) * width + x] = pack(c.r * vignette + noise, c.g * vignette + noise, c.b * vignette + noise);
				}
		}

		// The game's ICON0.PNG and PIC1.PNG. The installer writes them early;
		// while one is missing, it is looked for again every half second.
		void refresh_images(const std::string& dir)
		{
			const auto now = std::chrono::steady_clock::now();
			const bool retry = now - images_tried >= std::chrono::milliseconds(500);
			const auto try_art = [&]
			{
				image art;
				art_found = !dir.empty() && fs::is_file(dir + "/PIC1.PNG") && art.load(dir + "/PIC1.PNG");
				paint_sky(art_found ? &art : nullptr);
			};
			if (dir != art_dir)
			{
				art_dir = dir;
				images_tried = now;
				try_art();
			}
			else if (sky_width != frame.width || sky_height != frame.height || (!art_found && !dir.empty() && retry))
			{
				images_tried = now;
				const bool had_art = art_found;
				image art;
				art_found = !dir.empty() && fs::is_file(dir + "/PIC1.PNG") && art.load(dir + "/PIC1.PNG");
				if (art_found || had_art || sky_width != frame.width || sky_height != frame.height)
					paint_sky(art_found ? &art : nullptr);
			}
			if (dir != icon_dir)
			{
				icon_dir = dir;
				icon = {};
				if (!dir.empty() && fs::is_file(dir + "/ICON0.PNG"))
					icon.load(dir + "/ICON0.PNG");
				images_version++;
			}
			else if (icon.rgba.empty() && !dir.empty() && retry && fs::is_file(dir + "/ICON0.PNG"))
			{
				images_tried = now;
				if (icon.load(dir + "/ICON0.PNG"))
					images_version++;
			}
		}

		void draw_icon(float x, float y, float scale)
		{
			const float w = icon.width * scale, h = icon.height * scale, radius = 16 * scale;
			glow(frame, x, y + 14 * scale, w, h, radius, 22 * scale, black, 0.55f);
			glow(frame, x, y, w, h, radius, 30 * scale, glow_violet, 0.28f);
			shade_rounded(frame, x, y, w, h, radius, 1, [&](int px, int py, float d)
			{
				const float cover = std::clamp(0.5f - d, 0.f, 1.f);
				if (cover <= 0)
					return;
				// Frosted glass under an icon with transparency.
				frame.blend(px, py, white, 0.06f * cover);
				const auto texel = icon.sample((px + 0.5f - x) / scale, (py + 0.5f - y) / scale);
				frame.blend(px, py, {texel[0], texel[1], texel[2]}, cover * texel[3]);
				// A hairline on the edge.
				frame.blend(px, py, white, 0.14f * std::clamp(1.f - std::abs(d + 0.75f), 0.f, 1.f));
			});
		}

		void draw_bar(float x, float y, float w, float h, double progress, bool failed, float seconds, float scale)
		{
			const float radius = h / 2;
			const colour start = failed ? error_start : fill_start, end = failed ? error_end : fill_end;
			shade_rounded(frame, x, y, w, h, radius, 1, [&](int px, int py, float d)
			{
				frame.blend(px, py, white, 0.09f * std::clamp(0.5f - d, 0.f, 1.f));
			});
			float from = x, length = 0;
			if (progress >= 0)
			{
				length = static_cast<float>(std::clamp(progress, 0.0, 1.0)) * w;
				if (length > 0)
					length = std::max(length, h);
			}
			else
			{
				// Unknown amount: a segment gliding to and fro.
				length = w * 0.26f;
				from = x + (w - length) * (0.5f - 0.5f * std::cos(seconds * 2.1f));
			}
			if (length <= 0)
				return;
			glow(frame, from, y, length, h, radius, 12 * scale, start, 0.45f);
			// A highlight sweeping along the fill.
			const float sweep = from - 200 * scale + std::fmod(seconds * 520 * scale, length + 400 * scale);
			shade_rounded(frame, from, y, length, h, radius, 1, [&](int px, int py, float d)
			{
				const float cover = std::clamp(0.5f - d, 0.f, 1.f);
				if (cover <= 0)
					return;
				colour c = mix(start, end, std::clamp((px + 0.5f - x) / w, 0.f, 1.f));
				const float shine = (px + 0.5f - sweep) / (80 * scale);
				c = mix(c, white, 0.38f * std::exp(-shine * shine));
				// The upper half a touch lighter, as if lit from above.
				c = mix(c, white, 0.10f * std::clamp(1.f - (py + 0.5f - y) / h * 2.f, 0.f, 1.f));
				frame.blend(px, py, c, cover);
			});
		}

		struct layout
		{
			float scale, centre, heading_line, title_line, bar_left, bar_top, bar_width, bar_height, note_line;
		};

		layout place(bool with_icon)
		{
			layout l{};
			l.scale = frame.height / 1080.f;
			l.centre = frame.width / 2.f;
			l.heading_line = (with_icon ? 516 : 468) * l.scale;
			l.title_line = l.heading_line + 72 * l.scale;
			l.bar_width = 1000 * l.scale;
			l.bar_height = 12 * l.scale;
			l.bar_left = l.centre - l.bar_width / 2;
			l.bar_top = l.title_line + 64 * l.scale;
			l.note_line = l.bar_top + l.bar_height + 44 * l.scale;
			return l;
		}

		// The sky, the icon, the heading, the title and the footer: redrawn only
		// when one of them changes.
		void draw_layer(const screen& shown, const layout& l)
		{
			std::memcpy(frame.pixels.data(), sky.data(), sky.size() * sizeof(u32));
			if (!icon.rgba.empty())
			{
				const float icon_scale = l.scale * 320.f / icon.width;
				draw_icon(l.centre - icon.width * icon_scale / 2, 250 * l.scale, icon_scale);
			}

			std::u32string heading = decode(shown.heading);
			for (char32_t& c : heading)
				if (c >= U'a' && c <= U'z')
					c -= U'a' - U'A';
			const float heading_size = 21 * l.scale, heading_tracking = 4 * l.scale;
			draw_text(frame, bold, heading_size, heading, l.centre - measure(bold, heading_size, heading, heading_tracking) / 2,
				l.heading_line, shown.failed ? error_end : heading_colour, 1.f, heading_tracking);

			float title_size = 56 * l.scale;
			const std::u32string title = fit(bold, title_size, 34 * l.scale, decode(shown.title), 1400 * l.scale);
			draw_text(frame, bold, title_size, title, l.centre - measure(bold, title_size, title, 0) / 2, l.title_line, title_colour);

			if (!shown.serial.empty())
			{
				const std::u32string footer = decode(shown.serial);
				const float footer_size = 19 * l.scale, footer_tracking = 3 * l.scale;
				draw_text(frame, regular, footer_size, footer, l.centre - measure(regular, footer_size, footer, footer_tracking) / 2,
					frame.height - 56 * l.scale, faint_colour, 1.f, footer_tracking);
			}
			layer = frame.pixels;
		}

		void draw(const screen& shown)
		{
			find_fonts();
			refresh_images(shown.image_dir);
			const layout l = place(!icon.rgba.empty());
			const std::string key = shown.heading + '\n' + shown.title + '\n' + shown.serial + '\n' + std::to_string(shown.failed) + ' ' +
				std::to_string(images_version) + ' ' + std::to_string(frame.width) + 'x' + std::to_string(frame.height) + ' ' +
				std::to_string(regular.loaded) + std::to_string(bold.loaded);
			if (key != layer_key || layer.size() != frame.pixels.size())
			{
				draw_layer(shown, l);
				layer_key = key;
			}
			else
			{
				std::memcpy(frame.pixels.data(), layer.data(), layer.size() * sizeof(u32));
			}

			const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count();
			draw_bar(l.bar_left, l.bar_top, l.bar_width, l.bar_height, shown.progress, shown.failed, seconds, l.scale);

			const float note_size = 24 * l.scale;
			float detail_size = note_size;
			const std::u32string detail = fit(regular, detail_size, 18 * l.scale, decode(shown.detail), l.bar_width * 0.62f);
			draw_text(frame, regular, detail_size, detail, l.bar_left, l.note_line, muted_colour);
			const std::u32string remaining = decode(shown.remaining);
			draw_text(frame, bold, note_size, remaining, l.bar_left + l.bar_width - measure(bold, note_size, remaining, 0), l.note_line,
				title_colour, 0.92f);
		}

		// ---- Vulkan -------------------------------------------------------------

		struct slot
		{
			VkImage image = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			VkImageView view = VK_NULL_HANDLE;
			VkBuffer staging = VK_NULL_HANDLE;
			VkDeviceMemory staging_memory = VK_NULL_HANDLE;
			void* mapped = nullptr;
			VkCommandPool pool = VK_NULL_HANDLE;
			VkCommandBuffer commands = VK_NULL_HANDLE;
			retro_vulkan_image handed{};
			u32 width = 0;
			u32 height = 0;
		};

		// One per RetroArch frame index; RetroArch keeps pointers to `handed`.
		std::array<slot, 32> slots;

		u32 memory_type(VkPhysicalDevice gpu, u32 allowed, VkMemoryPropertyFlags wanted)
		{
			VkPhysicalDeviceMemoryProperties properties{};
			vkGetPhysicalDeviceMemoryProperties(gpu, &properties);
			for (u32 i = 0; i < properties.memoryTypeCount; i++)
				if ((allowed & (1u << i)) && (properties.memoryTypes[i].propertyFlags & wanted) == wanted)
					return i;
			return umax;
		}

		void destroy(VkDevice device, slot& s)
		{
			if (s.pool)
				vkDestroyCommandPool(device, s.pool, nullptr);
			if (s.view)
				vkDestroyImageView(device, s.view, nullptr);
			if (s.image)
				vkDestroyImage(device, s.image, nullptr);
			if (s.memory)
				vkFreeMemory(device, s.memory, nullptr);
			if (s.staging)
				vkDestroyBuffer(device, s.staging, nullptr);
			if (s.staging_memory)
				vkFreeMemory(device, s.staging_memory, nullptr);
			s = {};
		}

		bool create(retro_hw_render_interface_vulkan& vulkan, slot& s, u32 width, u32 height)
		{
			const VkDevice device = vulkan.device;
			s.width = width;
			s.height = height;

			const VkImageCreateInfo image_info
			{
				.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
				.imageType = VK_IMAGE_TYPE_2D,
				.format = VK_FORMAT_B8G8R8A8_UNORM,
				.extent = {width, height, 1},
				.mipLevels = 1,
				.arrayLayers = 1,
				.samples = VK_SAMPLE_COUNT_1_BIT,
				.tiling = VK_IMAGE_TILING_OPTIMAL,
				.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
				.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
				.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			};
			if (vkCreateImage(device, &image_info, nullptr, &s.image) != VK_SUCCESS)
				return false;
			VkMemoryRequirements needs{};
			vkGetImageMemoryRequirements(device, s.image, &needs);
			VkMemoryAllocateInfo allocation
			{
				.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
				.allocationSize = needs.size,
				.memoryTypeIndex = memory_type(vulkan.gpu, needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
			};
			if (allocation.memoryTypeIndex == umax || vkAllocateMemory(device, &allocation, nullptr, &s.memory) != VK_SUCCESS ||
				vkBindImageMemory(device, s.image, s.memory, 0) != VK_SUCCESS)
				return false;

			s.handed.create_info =
			{
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.image = s.image,
				.viewType = VK_IMAGE_VIEW_TYPE_2D,
				.format = VK_FORMAT_B8G8R8A8_UNORM,
				.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A},
				.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
			};
			if (vkCreateImageView(device, &s.handed.create_info, nullptr, &s.view) != VK_SUCCESS)
				return false;
			s.handed.image_view = s.view;
			s.handed.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

			const VkBufferCreateInfo buffer_info
			{
				.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
				.size = VkDeviceSize{width} * height * 4,
				.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
				.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			};
			if (vkCreateBuffer(device, &buffer_info, nullptr, &s.staging) != VK_SUCCESS)
				return false;
			vkGetBufferMemoryRequirements(device, s.staging, &needs);
			allocation.allocationSize = needs.size;
			allocation.memoryTypeIndex = memory_type(vulkan.gpu, needs.memoryTypeBits,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
			if (allocation.memoryTypeIndex == umax || vkAllocateMemory(device, &allocation, nullptr, &s.staging_memory) != VK_SUCCESS ||
				vkBindBufferMemory(device, s.staging, s.staging_memory, 0) != VK_SUCCESS ||
				vkMapMemory(device, s.staging_memory, 0, VK_WHOLE_SIZE, 0, &s.mapped) != VK_SUCCESS)
				return false;

			const VkCommandPoolCreateInfo pool_info
			{
				.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
				.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
				.queueFamilyIndex = vulkan.queue_index,
			};
			if (vkCreateCommandPool(device, &pool_info, nullptr, &s.pool) != VK_SUCCESS)
				return false;
			const VkCommandBufferAllocateInfo command_info
			{
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				.commandPool = s.pool,
				.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
				.commandBufferCount = 1,
			};
			return vkAllocateCommandBuffers(device, &command_info, &s.commands) == VK_SUCCESS;
		}
	}

	void set_font_dirs(std::vector<std::string> dirs)
	{
		font_dirs = std::move(dirs);
		fonts_tried = {};
	}

	bool active()
	{
		return std::any_of(slots.begin(), slots.end(), [](const slot& s) { return s.image != VK_NULL_HANDLE; });
	}

	void present(retro_hw_render_interface_vulkan& vulkan, retro_video_refresh_t video, const screen& shown, unsigned width, unsigned height)
	{
		// 1080p at most (RetroArch scales it), at the frame's 16:9.
		const u32 h = std::min(1080u, height), w = std::min(width, h * 16 / 9);
		if (frame.width != w || frame.height != h)
		{
			frame.width = w;
			frame.height = h;
			frame.pixels.assign(static_cast<usz>(w) * h, 0);
		}
		draw(shown);

#ifdef RPCS3_VULKAN_VOLK
		// Device functions come through volk, which RPCS3 loads only when its
		// renderer starts: before that, for RetroArch's device, here.
		static VkDevice loaded = VK_NULL_HANDLE;
		if (loaded != vulkan.device)
		{
			volkLoadDevice(vulkan.device);
			loaded = vulkan.device;
		}
#endif
		const u32 index = vulkan.get_sync_index(vulkan.handle);
		if (index >= slots.size())
			return;
		// The frame that last used this index is finished with its image.
		vulkan.wait_sync_index(vulkan.handle);
		slot& s = slots[index];
		if (s.image && (s.width != w || s.height != h))
			destroy(vulkan.device, s);
		if (!s.image && !create(vulkan, s, w, h))
		{
			destroy(vulkan.device, s);
			video(nullptr, w, h, 0);
			return;
		}
		std::memcpy(s.mapped, frame.pixels.data(), frame.pixels.size() * sizeof(u32));

		vkResetCommandPool(vulkan.device, s.pool, 0);
		const VkCommandBufferBeginInfo begin{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
		vkBeginCommandBuffer(s.commands, &begin);
		VkImageMemoryBarrier barrier
		{
			.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
			.srcAccessMask = 0,
			.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
			.image = s.image,
			.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		vkCmdPipelineBarrier(s.commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		const VkBufferImageCopy copy
		{
			.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
			.imageExtent = {w, h, 1},
		};
		vkCmdCopyBufferToImage(s.commands, s.staging, s.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		vkCmdPipelineBarrier(s.commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		vkEndCommandBuffer(s.commands);

		// RetroArch submits the copy before its own frame, which samples the image.
		vulkan.set_command_buffers(vulkan.handle, 1, &s.commands);
		vulkan.set_image(vulkan.handle, &s.handed, 0, nullptr, VK_QUEUE_FAMILY_IGNORED);
		// RETRO_HW_FRAME_BUFFER_VALID, without the macro's C cast.
		video(reinterpret_cast<const void*>(~uptr{0}), w, h, 0);
	}

	void release(retro_hw_render_interface_vulkan& vulkan)
	{
		if (!active())
			return;
		vulkan.lock_queue(vulkan.handle);
		vkQueueWaitIdle(vulkan.queue);
		vulkan.unlock_queue(vulkan.handle);
		for (slot& s : slots)
			destroy(vulkan.device, s);
	}
}
