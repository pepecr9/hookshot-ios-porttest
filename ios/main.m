// Minimal UIKit shell (scene-based, iOS 26): runs one test (PT_ENTRY) on a background thread, shows the report,
// writes Documents/<PT_OUTFILE> (Files > On My iPhone > the app) and offers a Share button to export it.
#import <UIKit/UIKit.h>
#ifndef PT_ENTRY
#define PT_ENTRY pt_all
#endif
#ifndef PT_OUTFILE
#define PT_OUTFILE "porttest.txt"
#endif
int PT_ENTRY(char *buf, unsigned long cap, int states, int touch);

@interface PTScene : UIResponder <UIWindowSceneDelegate>
@property (strong, nonatomic) UIWindow *window;
@property (strong, nonatomic) NSString *path;
@end

@implementation PTScene
- (void)share:(UIBarButtonItem *)sender {
    if (!self.path) return;
    UIActivityViewController *a = [[UIActivityViewController alloc] initWithActivityItems:@[[NSURL fileURLWithPath:self.path]] applicationActivities:nil];
    a.popoverPresentationController.barButtonItem = sender;
    [self.window.rootViewController presentViewController:a animated:YES completion:nil];
}
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)opts {
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    UIViewController *vc = [UIViewController new];
    vc.title = @PT_OUTFILE;
    UITextView *tv = [[UITextView alloc] initWithFrame:vc.view.bounds];
    tv.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    tv.editable = NO;
    tv.font = [UIFont monospacedSystemFontOfSize:9 weight:UIFontWeightRegular];
    tv.text = @"Running... keep the app in front (one to a few minutes).";
    [vc.view addSubview:tv];
    UIBarButtonItem *share = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemAction target:self action:@selector(share:)];
    share.enabled = NO;
    vc.navigationItem.rightBarButtonItem = share;
    self.window.rootViewController = [[UINavigationController alloc] initWithRootViewController:vc];
    [self.window makeKeyAndVisible];
    static int started;
    if (started++) return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        static char buf[1 << 20];
        int bad = PT_ENTRY(buf, sizeof buf, 256, 1);
        NSString *s = [NSString stringWithFormat:@"%s\nRESULT: %@\n", buf, bad ? @"FAILURES (see above)" : @"all checks passed"];
        NSString *doc = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
        NSString *path = [doc stringByAppendingPathComponent:@PT_OUTFILE];
        [s writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
        dispatch_async(dispatch_get_main_queue(), ^{ tv.text = s; self.path = path; share.enabled = YES; });
    });
}
@end

@interface PTApp : UIResponder <UIApplicationDelegate>
@end
@implementation PTApp
@end

int main(int argc, char *argv[]) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass([PTApp class])); }
}
