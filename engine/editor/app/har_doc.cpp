#include "assets.h"
#include "har.h"
#include "icons.h"

class HarDocument : public Document {
public:
	HarDocument(Editor &, const ed::Guid &g) : guid_(g) {}
	std::string title(Editor &) const override { return "HAR"; }
	const char *icon() const override { return ICON_BOT; }
	bool alive(Editor &e) const override { return e.find_object(guid_) >= 0; }
	bool addon_content() const override { return true; }
	bool shows(const ObjectRef &r) const override { return r.kind == ObjectRef::Addon && r.guid == guid_; }
	void draw(Editor &) override {}

private:
	ed::Guid guid_;
};

std::unique_ptr<Document> make_har_document(Editor &e, const ed::Guid &description) { return std::make_unique<HarDocument>(e, description); }
