#include "tests/includes/test_framework.h"

#include "settings/native/native_settings_controls.h"

#include "msimeui/Window.h"

#include <functional>
#include <memory>

namespace
{
using native_settings::Button;

// Counts live copies of itself and destroys the visual that owns the stored callback, the way a
// handler that closes its modal dialog does. Reading only after teardown is the point: a handler
// invoked through its owner's member is destroyed mid-call, a handler invoked from a copy is not.
struct HandlerProbe
{
    static int live;
    static int liveDuringCall;

    explicit HandlerProbe(std::function<void()> *owner) : owner_(owner)
    {
        ++live;
    }

    HandlerProbe(const HandlerProbe &other) : owner_(other.owner_)
    {
        ++live;
    }

    HandlerProbe &operator=(const HandlerProbe &) = delete;

    ~HandlerProbe()
    {
        --live;
    }

    void operator()() const
    {
        (*owner_)();
        liveDuringCall = live;
    }

  private:
    std::function<void()> *owner_;
};

int HandlerProbe::live = 0;
int HandlerProbe::liveDuringCall = -1;

void ResetProbe()
{
    HandlerProbe::live = 0;
    HandlerProbe::liveDuringCall = -1;
}

void CheckHandlerLifetime(WPARAM key)
{
    // No HWND is created, so these tests do not open a window or take input focus
    msimeui::Window window(L"settings-controls-tests", L"", 100, 100);
    auto button = std::make_shared<Button>(L"Delete");
    Button *raw = button.get();
    std::weak_ptr<Button> watch = button;
    raw->Attach(&window);
    raw->Arrange({0.0f, 0.0f, 100.0f, 30.0f});
    ResetProbe();
    std::function<void()> release = [&button]() { button.reset(); };
    raw->SetOnClick(HandlerProbe(&release));
    REQUIRE_EQ(HandlerProbe::live, 1);

    if (key)
        REQUIRE(raw->OnKeyDown(key, 0));
    else
    {
        const POINT point{10, 10};
        REQUIRE(raw->OnMouseDown(point, 0));
        REQUIRE(raw->OnMouseUp(point, 0));
    }

    REQUIRE(!button);
    REQUIRE(watch.expired());
    REQUIRE(HandlerProbe::liveDuringCall >= 1);
    REQUIRE_EQ(HandlerProbe::live, 0);
}
} // namespace

TEST_CASE(SettingsButtonMouseHandlerOutlivesTheDialogItCloses)
{
    CheckHandlerLifetime(0);
}

TEST_CASE(SettingsButtonEnterHandlerOutlivesTheDialogItCloses)
{
    CheckHandlerLifetime(VK_RETURN);
}

TEST_CASE(SettingsButtonSpaceHandlerOutlivesTheDialogItCloses)
{
    CheckHandlerLifetime(VK_SPACE);
}

TEST_CASE(SettingsLabelHandlerOutlivesTheVisualItDestroys)
{
    msimeui::Window window(L"settings-controls-tests", L"", 200, 200);
    auto label = std::make_shared<native_settings::Label>(L"打开", native_settings::BodyStyle());
    native_settings::Label *raw = label.get();
    raw->Attach(&window);
    raw->Arrange({0.0f, 0.0f, 100.0f, 30.0f});
    ResetProbe();
    std::function<void()> release = [&label]() { label.reset(); };
    raw->SetOnClick(HandlerProbe(&release));

    const POINT point{10, 10};
    REQUIRE(raw->OnMouseDown(point, 0));
    REQUIRE(raw->OnMouseUp(point, 0));

    REQUIRE(!label);
    REQUIRE(HandlerProbe::liveDuringCall >= 1);
    REQUIRE_EQ(HandlerProbe::live, 0);
}

TEST_CASE(SettingsButtonIgnoresDisabledAndOutsideClicks)
{
    msimeui::Window window(L"settings-controls-tests", L"", 100, 100);
    Button button(L"Delete");
    button.Attach(&window);
    button.Arrange({0.0f, 0.0f, 100.0f, 30.0f});
    int calls = 0;
    button.SetOnClick([&calls]() { ++calls; });
    const POINT inside{10, 10};
    const POINT outside{110, 10};

    button.SetDisabled(true);
    REQUIRE(!button.OnMouseDown(inside, 0));
    REQUIRE(!button.OnMouseUp(inside, 0));
    REQUIRE(!button.OnKeyDown(VK_RETURN, 0));
    REQUIRE(!button.OnKeyDown(VK_SPACE, 0));
    button.SetDisabled(false);
    REQUIRE(!button.OnMouseDown(outside, 0));
    REQUIRE(!button.OnMouseUp(inside, 0));
    REQUIRE(button.OnMouseDown(inside, 0));
    REQUIRE(button.OnMouseUp(outside, 0));
    REQUIRE(!button.OnKeyDown(VK_ESCAPE, 0));
    REQUIRE_EQ(calls, 0);

    REQUIRE(button.OnMouseDown(inside, 0));
    REQUIRE(button.OnMouseUp(inside, 0));
    REQUIRE(button.OnKeyDown(VK_RETURN, 0));
    REQUIRE(button.OnKeyDown(VK_SPACE, 0));
    REQUIRE_EQ(calls, 3);
}
