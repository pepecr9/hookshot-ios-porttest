// Minimal UIKit shell (scene-based, iOS 26): runs the porttest on a background thread, shows the report and writes
// Documents/porttest.txt (visible in Files > On My iPhone > HS PortTest).
#import <UIKit/UIKit.h>
int pt_all(char *buf, unsigned long cap, int states, int touch);

@interface PTScene : UIResponder <UIWindowSceneDelegate>
@property (strong, nonatomic) UIWindow *window;
@end

@implementation PTScene
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)opts {
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    UIViewController *vc = [UIViewController new];
    UITextView *tv = [[UITextView alloc] initWithFrame:vc.view.bounds];
    tv.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    tv.editable = NO;
    tv.font = [UIFont monospacedSystemFontOfSize:10 weight:UIFontWeightRegular];
    tv.text = @"Running Hookshot porttest... (about a minute)";
    [vc.view addSubview:tv];
    self.window.rootViewController = vc;
    [self.window makeKeyAndVisible];
    static int started;
    if (started++) return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        static char buf[1 << 20];
        int bad = pt_all(buf, sizeof buf, 256, 1);
        NSString *s = [NSString stringWithFormat:@"%s\nRESULT: %@\n", buf, bad ? @"lifter differences found" : @"lifter matches the CPU"];
        NSString *doc = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
        [s writeToFile:[doc stringByAppendingPathComponent:@"porttest.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
        dispatch_async(dispatch_get_main_queue(), ^{ tv.text = s; });
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
