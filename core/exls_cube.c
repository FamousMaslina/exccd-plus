#include "stddef.h"
#include "stdlib.h"
#include "string.h"
#include "exls_lut.h"

#define EXLS_FILE_MAX (4 * 1024 * 1024)
#define EXLS_LINE_MAX 512

static int digit(char c)
{
    return c >= '0' && c <= '9';
}

static const char *space(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r') p++;
    return p;
}

// Read a decimal as signed Q12. Six significant digits exceed table precision.
// Keeping the mantissa small bounds the conversion to unsigned 32-bit arithmetic.
static int decimal(const char **input, int *out)
{
    const char *p = space(*input);
    int neg = *p == '-';
    if (neg || *p == '+') p++;
    unsigned mantissa = 0;
    int significant = 0, places = 0, digits = 0, point = 0;
    while (digit(*p) || (*p == '.' && !point)) {
        if (*p == '.') {
            point = 1;
        } else {
            digits++;
            if (significant < 6) {
                mantissa = mantissa * 10 + (*p - '0');
                if (mantissa) significant++;
                places -= point;
            } else if (!point) {
                places++;
            }
        }
        p++;
    }
    if (!digits) return 0;
    if (*p == 'e' || *p == 'E') {
        p++;
        int down = *p == '-';
        if (down || *p == '+') p++;
        if (!digit(*p)) return 0;
        int exponent = 0;
        do {
            if (exponent > 100) return 0;
            exponent = exponent * 10 + (*p++ - '0');
        } while (digit(*p));
        places += down ? -exponent : exponent;
    }
    const char *end = space(p);
    if (*end && end == p) return 0;
    *input = end;
    if (!mantissa || places <= -10) {
        *out = 0;
        return 1;
    }
    if (places > 0) return 0;
    static const unsigned powers[] = {
        1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000
    };
    unsigned scale = powers[-places];
    if (scale <= 100000u && mantissa >= scale * 8) return 0;
    // The largest mantissa times 4096 still fits. Round without adding to the numerator.
    unsigned scaled = mantissa * 4096u;
    unsigned q = scaled / scale;
    q += scaled % scale >= (scale + 1) / 2;
    if (q > 32767) return 0;
    *out = neg ? -(int)q : (int)q;
    return 1;
}

static int header(const char **p, const char *keyword)
{
    const char *end = *p;
    while (*keyword)
        if (*end++ != *keyword++) return 0;
    const char *value = space(end);
    if (value == end) return 0;
    *p = value;
    return 1;
}

typedef struct {
    exls_cube *cube;
    unsigned count;
    int domain, title, range, seen_line;
    exls_node_fn node_fn;
    void *context;
} cube_parser;

static int valid_domain(const exls_cube *cube)
{
    return cube->min[0] < cube->max[0] && cube->min[1] < cube->max[1] &&
           cube->min[2] < cube->max[2];
}

static exls_result parse_line(cube_parser *parser, char *line)
{
    if (!parser->seen_line && (unsigned char)line[0] == 0xef &&
        (unsigned char)line[1] == 0xbb && (unsigned char)line[2] == 0xbf)
        line += 3;
    parser->seen_line = 1;
    int quoted = 0;
    for (char *c = line; *c; c++) {
        if (*c == '"') quoted = !quoted;
        if (*c == '#' && !quoted) {
            *c = 0;
            break;
        }
    }
    const char *p = space(line);
    if (!*p) return EXLS_OK;
    exls_cube *cube = parser->cube;
    if (!parser->count && header(&p, "TITLE")) {
        if (parser->title++ || *p++ != '"') return EXLS_INVALID;
        while (*p && *p != '"') p++;
        return *p && !*space(p + 1) ? EXLS_OK : EXLS_INVALID;
    }
    if (!parser->count && header(&p, "LUT_3D_SIZE")) {
        if (cube->size || !digit(*p)) return EXLS_INVALID;
        int size = *p++ - '0';
        if (digit(*p)) size = size * 10 + (*p++ - '0');
        if (*space(p) || (size != 9 && size != 17 && size != 32 && size != 33))
            return EXLS_INVALID;
        cube->size = size;
        return EXLS_OK;
    }
    int domain = 0;
    if (!parser->count) {
        if (header(&p, "DOMAIN_MIN")) domain = 1;
        else if (header(&p, "DOMAIN_MAX")) domain = 2;
        else if (header(&p, "LUT_3D_INPUT_RANGE")) {
            int lo, hi;
            if (parser->range || parser->domain || !decimal(&p, &lo) || !decimal(&p, &hi) || *p)
                return EXLS_INVALID;
            parser->range = 1;
            for (int i = 0; i < 3; i++) {
                cube->min[i] = lo;
                cube->max[i] = hi;
            }
            return EXLS_OK;
        }
    }
    if (domain && (parser->range || (parser->domain & domain))) return EXLS_INVALID;
    int size = cube->size;
    if (!domain && (!size || parser->count >= (unsigned)(size * size * size)))
        return EXLS_INVALID;
    int values[3];
    for (int i = 0; i < 3; i++)
        if (!decimal(&p, &values[i])) return EXLS_INVALID;
    if (*p) return EXLS_INVALID;
    if (domain) {
        parser->domain |= domain;
        memcpy(domain == 1 ? cube->min : cube->max, values, sizeof(values));
    } else {
        if (!parser->count && !valid_domain(cube)) return EXLS_INVALID;
        if (parser->node_fn) {
            exls_result result = parser->node_fn(parser->context, cube, parser->count, values);
            if (result != EXLS_OK) return result;
        }
        parser->count++;
    }
    return EXLS_OK;
}

exls_result exls_read_cube(exls_read_fn read_fn, void *context, exls_cube *cube,
                          exls_node_fn node_fn, void *node_context)
{
    *cube = (exls_cube){ .max = {4096, 4096, 4096} };
    cube_parser parser = { .cube = cube, .node_fn = node_fn, .context = node_context };
    unsigned char *block = malloc(EXLS_READ_SIZE + EXLS_LINE_MAX);
    if (!block) return EXLS_MEMORY;
    char *line = (char *)block + EXLS_READ_SIZE;
    unsigned total = 0, len = 0;
    exls_result result = EXLS_OK;
    int n;
    while (result == EXLS_OK && (n = read_fn(context, block, EXLS_READ_SIZE)) != 0) {
        if (n < 0 || n > EXLS_READ_SIZE) {
            result = EXLS_IO;
            break;
        }
        total += n;
        if (total > EXLS_FILE_MAX) {
            result = EXLS_INVALID;
            break;
        }
        for (unsigned i = 0; i < (unsigned)n && result == EXLS_OK; i++) {
            if (block[i] == '\n') {
                line[len] = 0;
                result = parse_line(&parser, line);
                len = 0;
            } else if (!block[i] || len == EXLS_LINE_MAX - 1) {
                result = EXLS_INVALID;
            } else {
                line[len++] = block[i];
            }
        }
    }
    if (result == EXLS_OK && len) {
        line[len] = 0;
        result = parse_line(&parser, line);
    }
    free(block);
    if (result == EXLS_OK) {
        int size = cube->size;
        if (!size || parser.count != (unsigned)(size * size * size))
            result = EXLS_INVALID;
    }
    return result;
}
