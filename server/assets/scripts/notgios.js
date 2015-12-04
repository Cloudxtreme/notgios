var notgios = angular.module('notgios', ['ngRoute', 'ngCookies']);

notgios.config(['$routeProvider', '$locationProvider', function ($routeProvider, $locationProvider) {
  $routeProvider.when('/', {
    templateUrl: '/templates/home.html',
    controller: 'homeController'
  }).when('/servers', {
    templateUrl: '/templates/servers.html',
    controller: 'serverController'
  }).when('/tasks', {
    templateUrl: '/templates/tasks.html',
    controller: 'taskController'
  }).when('/alarms', {
    templateUrl: '/templates/alarms.html',
    controller: 'alarmController'
  }).when('/contacts', {
    templateUrl: '/templates/contacts.html',
    controller: 'contactController'
  }).otherwise({
    redirectTo: '/'
  });

  $locationProvider.html5Mode(true);
}]);

notgios.factory('authenticated', ['$cookies', function ($cookies) {
  var authentication = {};
  authentication.loggedIn = function () {
    return $cookies.get('token') != null;
  };
  return authentication;
}]);

notgios.controller('homeController', ['$scope', '$http', function ($scope, $http) {

}]);

notgios.controller('serverController', ['$scope', '$http', function ($scope, $http) {

}]);

notgios.controller('taskController', ['$scope', '$http', function ($scope, $http) {

}]);

notgios.controller('alarmController', ['$scope', '$http', function ($scope, $http) {

}]);

notgios.controller('contactController', ['$scope', '$http', function ($scope, $http) {

}]);

notgios.controller('navbarController', ['$scope', '$http', '$route', '$cookies', 'authenticated', function ($scope, $http, $route, $cookies, authenticated) {

  $scope.loggedIn = authenticated.loggedIn;

  $scope.isActive = function (path) {
    if ($route.current && $route.current.regexp) return $route.current.regexp.test(path);
    return false;
  }

  $scope.login = function ($event) {
    $event.stopPropagation();
    if ($scope.user && $scope.pass) {
      $http({
        method: 'POST',
        url: 'sign_in',
        data: {
          username: $scope.user,
          password: $scope.pass
        }
      }).then(function success(response) {
        $scope.submissionError = '';
        $cookies.put('token', response.data)
      }, function failure(response) {
        if (response.status == 400) $scope.loginError = 'User does not exist.'
        else $scope.loginError = 'Password is incorrect';
      });
    } else {
      $scope.loginError = 'Please enter both a username and a password.';
    }
  };

  $scope.logout = function () {
    $cookies.remove('token');
  };

}]);

notgios.controller('signupController', ['$scope', '$http', '$cookies', 'authenticated', function ($scope, $http, $cookies, authenticated) {

  $scope.dropdown = 'Phone Number';

  $scope.loggedIn = authenticated.loggedIn;

  $scope.setDropdown = function (value) {
    $scope.dropdown = value;
    if (value == 'Phone Number') $scope.contactType = 'number';
    else $scope.contactType = 'email';
  }
  
  // I should have Angular do the form validation for me, but I don't have time to figure it out right now.
  $scope.signUp = function ($event) {
    if ($scope.username && $scope.username.length > 0 && $scope.password && $scope.password.length > 0) {
      if ($scope.password == $scope.passConfirm && $scope.contact && $scope.contact.length > 0) {
        $http({
          method: 'POST',
          url: '/sign_up',
          data: {
            username: $scope.username,
            password: $scope.password,
            contact: $scope.contact
          }
        }).then(function success(response) {
          $scope.submissionError = '';
          $cookies.put('token', response.data);
        }, function failure(response) {
          $event.stopPropagation();
          if (response.status == 400) $scope.submissionError = 'A user with that name already exists. Please pick another';
          else $scope.submissionError = 'There was an error during submission. Please check your internet and try again.';
        });
      } else {
        $event.stopPropagation();
        $scope.submissionError = 'The form did not pass validation. Please check your entries and try again.'
      }
    } else {
      $event.stopPropagation();
      $scope.submissionError = 'The form did not pass validation. Please check your entries and try again.'
    }
  };

}]);

notgios.directive('ngsNavbar', function () {
  return {
    templateUrl: '/templates/navbar.html',
    replace: true
  };
});

notgios.directive('ngsSignup', function () {
  return {
    templateUrl: '/templates/signup.html',
    replace: true
  };
});
