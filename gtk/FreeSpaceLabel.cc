// This file Copyright © 2008-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <memory>
#include <string>

#include <glibmm/i18n.h>

#include <libtransmission/utils.h>

#include "FreeSpaceLabel.h"
#include "Session.h"
#include "Utils.h"

class FreeSpaceLabel::Impl
{
public:
    Impl(FreeSpaceLabel& label, Glib::RefPtr<Session> const& core, std::string const& dir);
    ~Impl();

    TR_DISABLE_COPY_MOVE(Impl)

    void set_dir(std::string const& dir);

private:
    bool on_freespace_timer();

private:
    FreeSpaceLabel& label_;
    Glib::RefPtr<Session> const core_;
    std::string dir_;
    sigc::connection timer_id_;
};

FreeSpaceLabel::Impl::~Impl()
{
    timer_id_.disconnect();
}

bool FreeSpaceLabel::Impl::on_freespace_timer()
{
    if (core_->get_session() == nullptr)
    {
        return false;
    }

    auto const bytes = tr_dirSpace(dir_).free;
    auto const text = bytes < 0 ? _("Error") : gtr_sprintf(_("%s free"), tr_strlsize(bytes));
    auto const markup = gtr_sprintf("<i>%s</i>", text);
    label_.set_markup(markup);

    return true;
}

FreeSpaceLabel::FreeSpaceLabel(Glib::RefPtr<Session> const& core, std::string const& dir)
    : Gtk::Label()
    , impl_(std::make_unique<Impl>(*this, core, dir))
{
}

FreeSpaceLabel::~FreeSpaceLabel() = default;

FreeSpaceLabel::Impl::Impl(FreeSpaceLabel& label, Glib::RefPtr<Session> const& core, std::string const& dir)
    : label_(label)
    , core_(core)
    , dir_(dir)
{
    timer_id_ = Glib::signal_timeout().connect_seconds(sigc::mem_fun(*this, &Impl::on_freespace_timer), 3);
    on_freespace_timer();
}

void FreeSpaceLabel::set_dir(std::string const& dir)
{
    impl_->set_dir(dir);
}

void FreeSpaceLabel::Impl::set_dir(std::string const& dir)
{
    dir_ = dir;
    on_freespace_timer();
}
