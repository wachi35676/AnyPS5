#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_WINDOWSFORMATTING_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_WINDOWSFORMATTING_HPP

#include "SceTypes.hpp"
#include <array>
#include <bit>
#include <cerrno>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <climits>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <type_traits>

extern "C" int APS5_VABI strerror_r_nid_postfix(int error, char* buffer, std::size_t length);

namespace LibcDetail {

enum class ArgumentClass : unsigned char { Integer, Double, LongDouble };

class FormatArguments {
    VaList args;
    std::vector<std::array<unsigned char, 16>> numbered;
    size_t position = 1;

public:
    explicit FormatArguments(const void* source) {
        std::memcpy(&args, source, sizeof(args));
    }

    void Number(const std::vector<ArgumentClass>& classes) {
        numbered.resize(classes.size());
        for (size_t index = 0; index < classes.size(); ++index) {
            const auto store = [&](auto value) { std::memcpy(numbered[index].data(), &value, sizeof(value)); };
            if (classes[index] == ArgumentClass::LongDouble) store(Fetch<long double>());
            else if (classes[index] == ArgumentClass::Double) store(Fetch<double>());
            else store(Fetch<unsigned long long>());
        }
    }

    void Seek(size_t index) { position = index; }

    template<class T> T At(size_t index) {
        if (index == 0 || index > numbered.size()) throw std::out_of_range("Format argument position out of range");
        T value;
        std::memcpy(&value, numbered[index - 1].data(), sizeof(value));
        return value;
    }

    template<class T> T Next() {
        return numbered.empty() ? Fetch<T>() : At<T>(position++);
    }

private:
    template<class T> T Fetch() {
        const void* address;
        if constexpr (std::is_same_v<T, double>) {
            if (args.fp_offset < 176) {
                address = static_cast<const char*>(args.reg_save_area) + args.fp_offset;
                args.fp_offset += 16;
            } else {
                address = args.overflow_arg_area;
                args.overflow_arg_area = static_cast<char*>(args.overflow_arg_area) + 8;
            }
        } else if constexpr (std::is_same_v<T, long double>) {
            const auto aligned = (reinterpret_cast<std::uintptr_t>(args.overflow_arg_area) + 15) & ~std::uintptr_t(15);
            address = reinterpret_cast<const void*>(aligned);
            args.overflow_arg_area = reinterpret_cast<void*>(aligned + 16);
        } else {
            static_assert(sizeof(T) <= 8);
            if (args.gp_offset < 48) {
                address = static_cast<const char*>(args.reg_save_area) + args.gp_offset;
                args.gp_offset += 8;
            } else {
                address = args.overflow_arg_area;
                args.overflow_arg_area = static_cast<char*>(args.overflow_arg_area) + 8;
            }
        }
        T value;
        std::memcpy(&value, address, sizeof(value));
        return value;
    }

};

class FormatOutput {
    char* destination;
    size_t capacity;
    size_t count = 0;
    std::string* complete;

public:
    FormatOutput(char* buffer, size_t size, std::string* text) : destination(buffer), capacity(size), complete(text) {
        if (size && !buffer) throw std::invalid_argument("Null formatting buffer");
        if (capacity) destination[0] = 0;
    }

    void Append(const char* text, size_t size) {
        if (size > static_cast<size_t>(INT_MAX) - count)
            throw std::overflow_error("Formatted output exceeds INT_MAX");
        if (capacity && count < capacity - 1) {
            const size_t available = capacity - 1 - count;
            const size_t copied = size < available ? size : available;
            std::memcpy(destination + count, text, copied);
            destination[count + copied] = 0;
        }
        if (complete) complete->append(text, size);
        count += size;
    }

    template<class T> void Value(const std::string& format, T value) {
        const int size = std::snprintf(nullptr, 0, format.c_str(), value);
        if (size < 0) throw std::runtime_error("Formatting conversion failed");
        if (static_cast<size_t>(size) > static_cast<size_t>(INT_MAX) - count)
            throw std::overflow_error("Formatted output exceeds INT_MAX");
        if (complete) {
            std::vector<char> text(static_cast<size_t>(size) + 1);
            const int written = std::snprintf(text.data(), text.size(), format.c_str(), value);
            if (written != size) throw std::runtime_error("Inconsistent formatting conversion");
            complete->append(text.data(), static_cast<size_t>(size));
        }
        if (capacity && count < capacity - 1) {
            const size_t remaining = capacity - count;
            const size_t required = static_cast<size_t>(size) + 1;
            const int written = std::snprintf(destination + count, remaining < required ? remaining : required, format.c_str(), value);
            if (written != size) throw std::runtime_error("Inconsistent formatting conversion");
        }
        count += static_cast<size_t>(size);
    }

    int Count() const { return static_cast<int>(count); }
};

// The guest's wchar_t is 16 bits (UTF-16, as on the PS4/PS5 toolchain); wide conversions are
// formatted as UTF-8 through %s.
inline void AppendUtf8(std::string& utf8, char32_t code) {
    if (code > 0x10ffff) code = 0xfffd;
    if (code < 0x80) {
        utf8 += static_cast<char>(code);
    } else if (code < 0x800) {
        utf8 += static_cast<char>(0xc0 | (code >> 6));
        utf8 += static_cast<char>(0x80 | (code & 0x3f));
    } else if (code < 0x10000) {
        utf8 += static_cast<char>(0xe0 | (code >> 12));
        utf8 += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
        utf8 += static_cast<char>(0x80 | (code & 0x3f));
    } else {
        utf8 += static_cast<char>(0xf0 | (code >> 18));
        utf8 += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
        utf8 += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
        utf8 += static_cast<char>(0x80 | (code & 0x3f));
    }
}

inline std::string NanSpec(const std::string& spec) {
    std::string result = "%";
    std::size_t index = 1;
    for (; index < spec.size() && std::strchr("-+ #0", spec[index]); ++index)
        if (spec[index] == '-' && result.size() == 1) result += '-';
    for (; index < spec.size() && spec[index] >= '0' && spec[index] <= '9'; ++index) result += spec[index];
    return result + 's';
}

struct FormatFields {
    bool left = false;
    bool zero = false;
    bool plus = false;
    bool space = false;
    bool alternate = false;
    bool hasPrecision = false;
    std::size_t width = 0;
    std::size_t precision = 0;
};

inline FormatFields ReadFormatFields(const std::string& spec) {
    FormatFields fields;
    std::size_t index = 1;
    for (; index < spec.size() && std::strchr("-+ #0", spec[index]); ++index) {
        if (spec[index] == '-') fields.left = true;
        if (spec[index] == '0') fields.zero = true;
        if (spec[index] == '+') fields.plus = true;
        if (spec[index] == ' ') fields.space = true;
        if (spec[index] == '#') fields.alternate = true;
    }
    const auto readNumber = [&] {
        std::size_t number = 0;
        for (; index < spec.size() && spec[index] >= '0' && spec[index] <= '9'; ++index) {
            number = number * 10 + static_cast<std::size_t>(spec[index] - '0');
            if (number > static_cast<std::size_t>(INT_MAX)) throw std::overflow_error("Formatted output exceeds INT_MAX");
        }
        return number;
    };
    fields.width = readNumber();
    if (index < spec.size() && spec[index] == '.') {
        ++index;
        fields.hasPrecision = true;
        fields.precision = readNumber();
    }
    return fields;
}

inline std::string Justify(const FormatFields& fields, const std::string& prefix, const std::string& body, bool zero) {
    const std::size_t used = prefix.size() + body.size();
    const std::size_t padding = fields.width > used ? fields.width - used : 0;
    if (fields.left) return prefix + body + std::string(padding, ' ');
    if (zero) return prefix + std::string(padding, '0') + body;
    return std::string(padding, ' ') + prefix + body;
}

inline std::string FormatPointer(const std::string& spec, const void* pointer) {
    const FormatFields fields = ReadFormatFields(spec);
    const auto value = static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(pointer));
    std::string digits;
    for (unsigned long long rest = value; rest != 0; rest >>= 4) digits.insert(digits.begin(), "0123456789abcdef"[rest & 15]);
    if (digits.empty() && !(fields.hasPrecision && fields.precision == 0)) digits = "0";
    if (digits.size() < fields.precision) digits.insert(0, fields.precision - digits.size(), '0');
    return Justify(fields, "0x", digits, fields.zero && !fields.hasPrecision);
}

inline std::string HexFloatText(const std::string& spec, bool upper, bool binary64, bool negative, bool infinite, int exponent, std::uint64_t significand) {
    const FormatFields fields = ReadFormatFields(spec);
    std::string prefix = negative ? "-" : fields.plus ? "+" : fields.space ? " " : "";
    if (infinite) return Justify(fields, prefix, upper ? "INF" : "inf", false);
    prefix += upper ? "0X" : "0x";
    std::string body(1, significand != 0 ? '1' : '0');
    if (significand != 0) {
        const int shift = std::countl_zero(significand);
        significand <<= shift;
        exponent -= shift;
    } else {
        exponent = 0;
    }
    std::uint64_t fraction = significand << 1;
    if (fields.hasPrecision && fields.precision < 16) {
        const unsigned kept = binary64 && fields.precision == 13 ? 51 : 4 * static_cast<unsigned>(fields.precision);
        const std::uint64_t mask = ~std::uint64_t{0} >> kept;
        const std::uint64_t rest = fraction & mask;
        const std::uint64_t half = (mask >> 1) + 1;
        const bool odd = kept == 0 || ((fraction >> (64 - kept)) & 1) != 0;
        const int rounding = std::fegetround();
        bool up = rest > half || (rest == half && odd);
        if (rounding == FE_TOWARDZERO) up = false;
        else if (rounding == FE_UPWARD) up = rest != 0 && !negative;
        else if (rounding == FE_DOWNWARD) up = rest != 0 && negative;
        fraction &= ~mask;
        if (up) {
            fraction += mask + 1;
            if (fraction == 0) ++exponent;
        }
    }
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string fractionDigits;
    for (int shift = 60; shift >= 0; shift -= 4) fractionDigits += digits[(fraction >> shift) & 15];
    if (fields.hasPrecision) fractionDigits.resize(fields.precision, '0');
    else fractionDigits.erase(fractionDigits.find_last_not_of('0') + 1);
    if (!fractionDigits.empty() || fields.alternate) body += '.';
    body += fractionDigits;
    body += upper ? 'P' : 'p';
    body += exponent < 0 ? '-' : '+';
    body += std::to_string(exponent < 0 ? -exponent : exponent);
    return Justify(fields, prefix, body, fields.zero);
}

inline std::string FormatHexFloat(const std::string& spec, bool upper, double value) {
    const auto bits = std::bit_cast<std::uint64_t>(value);
    const int biased = static_cast<int>((bits >> 52) & 0x7ff);
    const std::uint64_t significand = ((bits << 12) >> 1) | (biased != 0 ? std::uint64_t{1} << 63 : 0);
    return HexFloatText(spec, upper, true, (bits >> 63) != 0, biased == 0x7ff, biased != 0 ? biased - 1023 : -1022, significand);
}

inline std::string FormatHexFloat(const std::string& spec, bool upper, long double value) {
    std::uint64_t significand;
    std::uint16_t sign;
    std::memcpy(&significand, &value, sizeof(significand));
    std::memcpy(&sign, reinterpret_cast<const unsigned char*>(&value) + sizeof(significand), sizeof(sign));
    const int biased = sign & 0x7fff;
    return HexFloatText(spec, upper, false, (sign >> 15) != 0, biased == 0x7fff, biased != 0 ? biased - 16383 : -16382, significand);
}

constexpr size_t maximumArgumentPosition = 99;

inline size_t ArgumentPosition(const char*& format) {
    const char* end = format;
    while (*end >= '0' && *end <= '9') ++end;
    if (end == format || *end != '$') return 0;
    size_t position = 0;
    for (const char* digit = format; digit != end && position <= maximumArgumentPosition; ++digit)
        position = position * 10 + static_cast<size_t>(*digit - '0');
    if (position == 0 || position > maximumArgumentPosition) throw std::invalid_argument("Format argument position out of range");
    format = end + 1;
    return position;
}

inline std::vector<ArgumentClass> NumberedArgumentClasses(const char* format) {
    if (!std::strchr(format, '$')) return {};
    std::vector<ArgumentClass> classes;
    bool numbered = false;
    size_t next = 1;
    const auto take = [&](size_t position, ArgumentClass type) {
        if (classes.size() < position) classes.resize(position);
        classes[position - 1] = type;
    };
    while ((format = std::strchr(format, '%')) != nullptr) {
        ++format;
        if (*format == '%') {
            ++format;
            continue;
        }
        if (const size_t position = ArgumentPosition(format)) {
            next = position;
            numbered = true;
        }
        while (*format && std::strchr("-+ #0'123456789.*", *format)) {
            if (*format++ != '*') continue;
            const size_t position = ArgumentPosition(format);
            if (position) numbered = true;
            take(position ? position : next++, ArgumentClass::Integer);
        }
        bool longDouble = false;
        while (*format && std::strchr("hljztL", *format)) longDouble = *format++ == 'L';
        const char conversion = *format;
        if (!conversion) break;
        ++format;
        if (std::strchr("aAeEfFgG", conversion)) take(next++, longDouble ? ArgumentClass::LongDouble : ArgumentClass::Double);
        else if (std::strchr("diouxXcCsSpn", conversion)) take(next++, ArgumentClass::Integer);
    }
    if (!numbered) classes.clear();
    return classes;
}

inline int FormatWindows(char* buffer, size_t size, const char* format, const void* source, std::string* complete = nullptr) {
    if (!format || !source) throw std::invalid_argument("Null formatting argument");
    const int savedError = errno;
    const char* const formatStart = format;
    FormatArguments args(source);
    args.Number(NumberedArgumentClasses(format));
    FormatOutput output(buffer, size, complete);
    const auto star = [&] {
        const size_t position = ArgumentPosition(format);
        return position ? args.At<int>(position) : args.Next<int>();
    };
    while (*format) {
        const char* literal = format;
        while (*format && *format != '%') ++format;
        output.Append(literal, static_cast<size_t>(format - literal));
        if (!*format) break;
        ++format;
        if (*format == '%') {
            output.Append(format++, 1);
            continue;
        }
        if (const size_t position = ArgumentPosition(format)) args.Seek(position);
        std::string spec = "%";
        for (; *format && std::strchr("-+ #0'", *format); ++format)
            if (*format != '\'') spec += *format;
        if (*format == '*') {
            ++format;
            const int width = star();
            if (width < 0) spec += '-';
            spec += std::to_string(width < 0 ? -static_cast<long long>(width) : width);
        } else {
            while (*format >= '0' && *format <= '9') spec += *format++;
        }
        size_t precisionLimit = std::numeric_limits<size_t>::max();
        if (*format == '.') {
            ++format;
            if (*format == '*') {
                ++format;
                const int precision = star();
                if (precision >= 0) {
                    spec += "." + std::to_string(precision);
                    precisionLimit = static_cast<size_t>(precision);
                }
            } else {
                spec += '.';
                precisionLimit = 0;
                while (*format >= '0' && *format <= '9') {
                    const auto digit = static_cast<size_t>(*format - '0');
                    precisionLimit = precisionLimit <= (std::numeric_limits<size_t>::max() - digit) / 10
                        ? precisionLimit * 10 + digit : std::numeric_limits<size_t>::max();
                    spec += *format++;
                }
            }
        }
        std::string length;
        if (*format && std::strchr("hljztL", *format)) {
            length += *format++;
            if ((length == "h" && *format == 'h') || (length == "l" && *format == 'l'))
                length += *format++;
        }
        const char conversion = *format;
        if (!conversion) throw std::invalid_argument("Incomplete format conversion");
        ++format;
        const bool integerLength = length.empty() || length == "h" || length == "hh" ||
            length == "l" || length == "ll" || length == "j" || length == "z" || length == "t";
        if (conversion == 'd' || conversion == 'i') {
            if (!integerLength) throw std::invalid_argument("Invalid integer length");
            long long value;
            if (length.empty() || length == "h" || length == "hh") {
                value = args.Next<int>();
                if (length == "h") value = static_cast<short>(value);
                if (length == "hh") value = static_cast<signed char>(value);
            } else value = args.Next<long long>();
            output.Value(spec + "ll" + conversion, value);
        } else if (std::strchr("ouxX", conversion)) {
            if (!integerLength) throw std::invalid_argument("Invalid integer length");
            unsigned long long value;
            if (length.empty() || length == "h" || length == "hh") {
                value = args.Next<unsigned int>();
                if (length == "h") value = static_cast<unsigned short>(value);
                if (length == "hh") value = static_cast<unsigned char>(value);
            } else value = args.Next<unsigned long long>();
            if (conversion == 'o' && value == 0 && precisionLimit == 0 && spec.find('#') != std::string::npos)
                spec = spec.substr(0, spec.find('.')) + ".1";
            output.Value(spec + "ll" + conversion, value);
        } else if (std::strchr("aAeEfFgG", conversion)) {
            const char* nan = conversion >= 'a' ? "nan" : "NAN";
            const bool hexadecimal = conversion == 'a' || conversion == 'A';
            const auto appendHexadecimal = [&](auto value) {
                const std::string text = FormatHexFloat(spec, conversion == 'A', value);
                output.Append(text.data(), text.size());
            };
            if (length == "L") {
                static_assert(sizeof(long double) == 16);
                static_assert(std::numeric_limits<long double>::digits == 64);
                const long double value = args.Next<long double>();
                if (std::isnan(value)) output.Value(NanSpec(spec), nan);
                else if (hexadecimal) appendHexadecimal(value);
                else output.Value(spec + "L" + conversion, value);
            } else {
                if (!length.empty() && length != "l") throw std::invalid_argument("Invalid floating length");
                const double value = args.Next<double>();
                if (std::isnan(value)) output.Value(NanSpec(spec), nan);
                else if (hexadecimal) appendHexadecimal(value);
                else output.Value(spec + conversion, value);
            }
        } else if (conversion == 'c' && (length.empty() || length == "h")) {
            output.Value(spec + conversion, args.Next<int>());
        } else if ((conversion == 'c' && length == "l") || (conversion == 'C' && length.empty())) {
            std::string utf8;
            AppendUtf8(utf8, static_cast<char16_t>(args.Next<unsigned int>()));
            if (utf8[0] == 0) output.Value(spec + 'c', 0);
            else output.Value(spec + 's', utf8.c_str());
        } else if (conversion == 's' && (length.empty() || length == "h")) {
            const char* value = args.Next<const char*>();
            if (!value) value = "(null)";
            output.Value(spec + 's', value);
        } else if ((conversion == 's' && length == "l") || (conversion == 'S' && length.empty())) {
            const char16_t* value = args.Next<const char16_t*>();
            if (!value) value = u"(null)";
            std::string utf8;
            while (utf8.size() < precisionLimit && *value) {
                char32_t code = *value++;
                if (code >= 0xd800 && code < 0xdc00 && *value >= 0xdc00 && *value < 0xe000) code = 0x10000 + ((code - 0xd800) << 10) + (*value++ - 0xdc00);
                const auto previousSize = utf8.size();
                AppendUtf8(utf8, code);
                if (utf8.size() > precisionLimit) {
                    utf8.resize(previousSize);
                    break;
                }
            }
            output.Value(spec + 's', utf8.c_str());
        } else if (conversion == 'p' && length.empty()) {
            const std::string text = FormatPointer(spec, args.Next<void*>());
            output.Append(text.data(), text.size());
        } else if (conversion == 'm') {
            char message[128];
            strerror_r_nid_postfix(savedError, message, sizeof(message));
            output.Value(spec + 's', static_cast<const char*>(message));
        } else if (conversion == 'n' && integerLength && spec == "%") {
            void* pointer = args.Next<void*>();
            if (!pointer) throw std::invalid_argument("Null format count pointer");
            const int count = output.Count();
            if (length == "hh") *static_cast<signed char*>(pointer) = static_cast<signed char>(count);
            else if (length == "h") *static_cast<short*>(pointer) = static_cast<short>(count);
            else if (length.empty()) *static_cast<int*>(pointer) = count;
            else *static_cast<long long*>(pointer) = count;
        } else {
            throw std::invalid_argument("Unsupported format conversion '" + spec + length + conversion + "' in \"" + std::string(formatStart).substr(0, 160) + "\"");
        }
    }
    return output.Count();
}

inline int PrintWindows(const char* format, const void* args) {
    std::string buffer;
    const int size = FormatWindows(nullptr, 0, format, args, &buffer);
    if (std::fwrite(buffer.data(), 1, static_cast<size_t>(size), stdout) != static_cast<size_t>(size))
        throw std::runtime_error("Formatted output write failed");
    return size;
}

}

#endif
