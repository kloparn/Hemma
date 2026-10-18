/* tests/harness.c — see harness.h */
#include "harness.h"

static Buf page_buf;

void th_boot(void) { th_boot_in(test_tmpdir()); }

void th_boot_in(const char *dir) {
    app_set_testing(true);
    if (!app_boot(dir)) test_fail(__FILE__, __LINE__, "app_boot failed");
}

void th_halt(void) { app_halt(); }

int th_person(const char *name) {
    Person p = {0};
    str_copy(p.name, sizeof p.name, name);
    p.hue = 200;
    p.created = now_sec();
    tbl_put(&T_person, &p);
    return p.id;
}

const char *th_page(int person, const char *url) {
    Ctx c;
    app_ctx(&c, person, url);
    buf_reset(&page_buf);
    app_render(&c, &page_buf);
    app_ctx_free(&c);
    return buf_cstr(&page_buf);
}

ThResult th_act(int person, const char *action, const char *form) {
    Ctx c;
    app_ctx(&c, person, "/");
    Fields f = {0};
    if (form) fields_parse(&f, form, strlen(form));
    ThResult r = {0};
    bool found = app_action(&c, action, &f);
    r.ok = found && !c.failed;
    str_copy(r.toast, sizeof r.toast, c.toast);
    str_copy(r.redirect, sizeof r.redirect, c.redirect);
    str_copy(r.field, sizeof r.field, c.fail_field);
    if (c.undo) snprintf(r.undo, sizeof r.undo, "%016llx", (unsigned long long)c.undo);
    fields_free(&f);
    app_ctx_free(&c);
    return r;
}
