/* modules/plan/shop.c — the shared shopping list (/plan/shop). */
#include "plan.h"

#define MAX_ITEMS 64

static Item *item_named(const char *text, bool include_done) {
    char want[100], have[100];
    fold_copy(want, sizeof want, text);
    for (int i = 0; i < tbl_count(&T_item); i++) {
        Item *it = tbl_at(&T_item, i);
        if (it->done && !include_done) continue;
        fold_copy(have, sizeof have, it->text);
        if (str_eq(want, have)) return it;
    }
    return NULL;
}

static void record_bought(const char *text, int64_t now) {
    char key[100];
    fold_copy(key, sizeof key, text);
    Bought rec = {0};
    for (int i = 0; i < tbl_count(&T_bought); i++) {
        Bought *x = tbl_at(&T_bought, i);
        if (str_eq(x->key, key)) {
            rec = *x;
            break;
        }
    }
    str_copy(rec.key, sizeof rec.key, key);
    str_copy(rec.text, sizeof rec.text, text);
    rec.count++;
    rec.last = now;
    tbl_put(&T_bought, &rec);
}

static int cmp_bought(const void *a, const void *b) {
    const Bought *x = tbl_at(&T_bought, *(const int *)a), *y = tbl_at(&T_bought, *(const int *)b);
    if (x->count != y->count) return y->count - x->count;
    return x->last == y->last ? 0 : (x->last > y->last ? -1 : 1);
}

/* Things bought before that aren't on the list (or in the basket) right now, most frequent first. */
static int suggestions(int out[], int max) {
    static int idx[1024];
    int n = 0;
    for (int i = 0; i < tbl_count(&T_bought) && n < countof(idx); i++) {
        Bought *x = tbl_at(&T_bought, i);
        if (!item_named(x->text, true)) idx[n++] = i;
    }
    qsort(idx, (size_t)n, sizeof *idx, cmp_bought);
    int k = MIN(n, max);
    memcpy(out, idx, (size_t)k * sizeof *out);
    return k;
}

static int cmp_done_item(const void *a, const void *b) {
    const Item *x = tbl_at(&T_item, *(const int *)a), *y = tbl_at(&T_item, *(const int *)b);
    if (x->done_at != y->done_at) return x->done_at > y->done_at ? -1 : 1;
    return y->id - x->id;
}

static void item_row(Buf *b, const Item *it) {
    H(b, "<li class=\"plan-shop-i%r\" id=\"plan-si%d\">", it->done ? " plan-done" : "", it->id);
    ui_form(b, "plan.shop_toggle", "inline plan-shop-f");
    ui_hidden_int(b, "id", it->id);
    H(b, "<button class=\"plan-shop-btn\" aria-pressed=\"%r\" aria-label=\"%s %s\"><span class=\"tick%r\">",
      it->done ? "true" : "false", it->done ? T("Put back:") : T("Got it:"), it->text, it->done ? " on" : "");
    ui_icon(b, "check");
    H(b, "</span><span class=\"plan-shop-t\">%s</span></button>", it->text);
    ui_form_end(b);
    Person *p = person_get(it->by);
    if (p && !it->done) {
        H(b, "<span class=\"plan-shop-by\"><span class=\"sr-only\">%s </span>", T("added by")); /* (issue #69) */
        ui_avatar(b, p, "xs");
        buf_puts(b, "</span>");
    }
    if (!it->done) {
        ui_form(b, "plan.shop_delete", "inline");
        ui_hidden_int(b, "id", it->id);
        char label[160];
        snprintf(label, sizeof label, T("Remove %s"), it->text);
        H(b, "<button class=\"icon-btn\" title=\"%s\" aria-label=\"%s\">", T("Remove"), label);
        ui_icon(b, "x");
        buf_puts(b, "</button>");
        ui_form_end(b);
    }
    buf_puts(b, "</li>");
}

static void preview(Ctx *c, Buf *b) {
    const char *q = ctx_live(c, "plan.sq");
    if (str_blank(q)) return;
    char items[MAX_ITEMS][100];
    int n = plan_split_items(q, items, MAX_ITEMS);
    if (!n) return;
    H(b, "<div class=\"preview ok\" data-say><span>%s</span>", T("Adding"));
    for (int i = 0; i < n; i++) {
        bool dup = item_named(items[i], false) != NULL;
        H(b, "<span class=\"chip%r\">%s", dup ? " plan-dup" : "", items[i]);
        if (dup) H(b, " <small>%s</small>", T("already on the list"));
        buf_puts(b, "</span>");
    }
    buf_puts(b, "</div>");
}

void plan_shop_page(Ctx *c, Buf *b) {
    ctx_title(c, "%s", T("Shopping"));
    Buf sub = {0};
    plan_subtitle(c, &sub);
    ui_head(b, T("Plan"), buf_cstr(&sub));
    plan_tabs(c, b, 1);
    ui_head_end(b);
    buf_free(&sub);

    ui_form(b, "plan.shop_add", "quick plan-quick");
    H(b, "<div class=\"quick-row\"><input name=\"q\" required maxlength=\"500\" autocomplete=\"off\" "
         "enterkeyhint=\"done\" data-live=\"plan.shop_preview\" data-key=\"/\" aria-label=\"%s\" placeholder=\"%s\">"
         "<button class=\"btn primary\" aria-label=\"%s\">", /* (on phones only the + shows: issue #65) */
      T("Add to the shopping list"), T("Add items — “milk, eggs, 2 bread”"), T("Add items"));
    ui_icon(b, "plus");
    H(b, "<span>%s</span></button></div>", T("Add"));
    preview(c, b);
    ui_form_end(b);

    int sug[10];
    int ns = suggestions(sug, countof(sug));
    if (ns) {
        H(b, "<div class=\"chips plan-shop-sug\" aria-label=\"%s\"><span class=\"plan-shop-sug-l\">%s</span>",
          T("Often bought"), T("Often bought"));
        Buf label = {0};
        for (int i = 0; i < ns; i++) {
            Bought *x = tbl_at(&T_bought, sug[i]);
            buf_reset(&label);
            H(&label, "<span aria-hidden=\"true\">+</span> %s", x->text);
            char title[140];
            snprintf(title, sizeof title, T("Add %s"), x->text);
            ui_act(b, "plan.shop_add", "chip", buf_cstr(&label), "q", x->text, title);
        }
        buf_free(&label);
        buf_puts(b, "</div>");
    }

    int open = plan_open_items();
    buf_puts(b, "<div class=\"plan-shop-cols\"><section class=\"card plan-shop-list\">");
    if (!open) {
        ui_empty(b, "🛒", T("The list is empty"), T("Add what you need — several things at once, separated by commas."));
    } else {
        H(b, "<header class=\"card-h\"><h2>%s <span class=\"plan-n\">%d</span></h2></header><ul class=\"plan-shop\">",
          T("To buy"), open);
        for (int i = 0; i < tbl_count(&T_item); i++) {
            Item *it = tbl_at(&T_item, i);
            if (!it->done) item_row(b, it);
        }
        buf_puts(b, "</ul>");
    }
    buf_puts(b, "</section>");

    static int idx[4096];
    int nd = 0;
    for (int i = 0; i < tbl_count(&T_item) && nd < countof(idx); i++)
        if (((Item *)tbl_at(&T_item, i))->done) idx[nd++] = i;
    if (nd) {
        qsort(idx, (size_t)nd, sizeof *idx, cmp_done_item);
        H(b, "<section class=\"card plan-shop-basket\"><header class=\"card-h\"><h2>%s <span "
             "class=\"plan-n\">%d</span></h2>",
          T("In the basket"), nd);
        Buf clear = {0}; /* (ui_act's label is HTML) */
        H(&clear, "%s", T("Clear"));
        ui_act(b, "plan.shop_clear", "btn sm", buf_cstr(&clear), NULL, NULL, T("Remove everything in the basket"));
        buf_free(&clear);
        buf_puts(b, "</header><ul class=\"plan-shop\">");
        for (int k = 0; k < nd && k < 40; k++) item_row(b, tbl_at(&T_item, idx[k]));
        buf_puts(b, "</ul></section>");
    }
    buf_puts(b, "</div>");
}

int plan_shop_items(Buf *b, int max) {
    int open = plan_open_items(), shown = 0;
    if (!open) return 0;
    buf_puts(b, "<ul class=\"plan-shop-mini\">");
    for (int i = 0; i < tbl_count(&T_item) && shown < max; i++) {
        Item *it = tbl_at(&T_item, i);
        if (it->done) continue;
        H(b, "<li>%s</li>", it->text);
        shown++;
    }
    buf_puts(b, "</ul>");
    if (open > max) {
        H(b, "<p class=\"muted plan-more\" data-fit-more=\"%d\">", open - max);
        H(b, T("+%d more"), open - max);
        buf_puts(b, "</p>");
    }
    return open;
}

void plan_shop_widget(Ctx *c, Buf *b) {
    (void)c;
    int open = plan_open_items();
    H(b, "<section class=\"card plan-w\"><header class=\"card-h\"><h2>%s", T("Shopping list"));
    if (open) H(b, " <span class=\"plan-n\">%d</span>", open);
    H(b, "</h2><a class=\"more\" href=\"/plan/shop\">%s</a></header>", T("Open"));
    if (!plan_shop_items(b, 8)) H(b, "<p class=\"muted plan-w-empty\">%s</p>", T("Nothing on the list"));
    buf_puts(b, "</section>");
}

/* ---------------------------------------------------------------- actions */

void plan_act_shop_preview(Ctx *c, Fields *f) { ctx_set_live(c, "plan.sq", fields_str(f, "q")); }

void plan_act_shop_add(Ctx *c, Fields *f) {
    char items[MAX_ITEMS][100];
    int n = plan_split_items(fields_str(f, "q"), items, MAX_ITEMS);
    if (!n) {
        ctx_fail(c, "%s", T("What do we need?"));
        return;
    }
    Buf added = {0};
    int count = 0, already = 0;
    for (int i = 0; i < n; i++) {
        if (item_named(items[i], false)) {
            already++;
            continue;
        }
        Item *back = item_named(items[i], true); /* in the basket: put it back on the list */
        Item it = back ? *back : (Item){0};
        str_copy(it.text, sizeof it.text, items[i]);
        it.done = false;
        it.done_at = 0;
        it.by = c->me->id;
        it.created = c->now;
        tbl_put(&T_item, &it);
        if (count++ < 3) buf_printf(&added, "%s%s", count > 1 ? ", " : "", items[i]);
    }
    ctx_set_live(c, "plan.sq", "");
    if (!count) {
        ctx_info(c, "%s", n == 1 ? T("Already on the list") : T("All of that is already on the list"));
        buf_free(&added);
        return;
    }
    char what[400]; /* the first three, "and N more" */
    if (count > 3) {
        snprintf(what, sizeof what, T("%s and %d more"), buf_cstr(&added), count - 3);
        activity_add(c->me->id, "🛒", "/plan/shop", N_("%s added %s and %d more to the shopping list"), c->me->name,
                     buf_cstr(&added), count - 3);
    } else {
        str_copy(what, sizeof what, buf_cstr(&added));
        activity_add(c->me->id, "🛒", "/plan/shop", N_("%s added %s to the shopping list"), c->me->name, buf_cstr(&added));
    }
    if (already) ctx_ok(c, T("Added %s · %d already on the list"), what, already);
    else ctx_ok(c, T("Added %s"), what);
    buf_free(&added);
}

void plan_act_shop_toggle(Ctx *c, Fields *f) {
    Item *row = tbl_get(&T_item, (int)fields_int(f, "id", 0));
    if (!row) {
        ctx_fail(c, "%s", T("That item is gone"));
        return;
    }
    Item it = *row;
    it.done = !it.done;
    it.done_at = it.done ? c->now : 0;
    tbl_put(&T_item, &it);
    if (it.done) record_bought(it.text, c->now);
}

void plan_act_shop_delete(Ctx *c, Fields *f) {
    Item *it = tbl_get(&T_item, (int)fields_int(f, "id", 0));
    if (!it) {
        ctx_fail(c, "%s", T("That item is gone"));
        return;
    }
    ctx_undo(c, &T_item, it->id); /* the × is instant: the toast's Undo puts it back (issue #60) */
    ctx_ok(c, T("Removed %s"), it->text);
    tbl_del(&T_item, it->id);
}

void plan_act_shop_clear(Ctx *c, Fields *f) {
    int n = 0;
    for (int i = tbl_count(&T_item) - 1; i >= 0; i--) {
        Item *it = tbl_at(&T_item, i);
        if (it->done) {
            ctx_undo(c, &T_item, it->id);
            tbl_del(&T_item, it->id);
            n++;
        }
    }
    if (n) ctx_ok(c, TN(n, "Cleared %d item", "Cleared %d items"), n);
    else ctx_info(c, "%s", T("The basket is already empty"));
}
