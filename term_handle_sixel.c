static void term_handle_sixel(Terminal *t, uint8_t b) {
    if (b == 0x1B || b == 0x9C) { /* Finish Sixel */
        if (t->sixel_ctx.pix) {
            GLuint tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
            uint32_t *rgba = malloc(t->sixel_ctx.w * t->sixel_ctx.h * 4);
            for (int i=0; i<t->sixel_ctx.w*t->sixel_ctx.h; i++) rgba[i] = t->sixel_ctx.pal[t->sixel_ctx.pix[i]];
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t->sixel_ctx.w, t->sixel_ctx.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            int sid = t->sixel_count % 64; t->sixels[sid] = (SixelImg){ (uint32_t)sid, (int)(t->cx*G_hw.font_w), (int)(t->cy*G_hw.font_h), t->sixel_ctx.w, t->sixel_ctx.h, tex, 1 };
            t->sixel_count++; free(rgba); free(t->sixel_ctx.pix); t->sixel_ctx.pix = NULL;
        }
        t->parser_state = ST_GROUND; return;
    }
    if (!t->sixel_ctx.pix) { t->sixel_ctx.w = 512; t->sixel_ctx.h = 512; t->sixel_ctx.pix = calloc(t->sixel_ctx.w * t->sixel_ctx.h, 1); t->sixel_ctx.x = 0; t->sixel_ctx.y = 0; }
    if (b >= 63 && b <= 126) {
        int sixel = b - 63;
        for (int i=0; i<6; i++) if (sixel & (1<<i)) {
            int px = t->sixel_ctx.x, py = t->sixel_ctx.y + i;
            if (px < t->sixel_ctx.w && py < t->sixel_ctx.h) t->sixel_ctx.pix[py * t->sixel_ctx.w + px] = t->sixel_ctx.cur_pal;
        }
        t->sixel_ctx.x++;
    } else if (b == '$') t->sixel_ctx.x = 0;
    else if (b == '-') { t->sixel_ctx.x = 0; t->sixel_ctx.y += 6; }
    else if (b == '#') { t->sixel_ctx.cur_pal = 0; }
}
