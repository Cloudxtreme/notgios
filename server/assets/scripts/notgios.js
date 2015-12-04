var notgios = angular.module('notgios', ['ngRoute']);

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

notgios.controller('navbarController', ['$scope', '$http', '$route', function ($scope, $http, $route) {

  $scope.isActive = function (path) {
    if ($route.current && $route.current.regexp) return $route.current.regexp.test(path);
    return false;
  }

  $scope.login = function () {

  };
}]);

notgios.controller('signupController', ['$scope', '$http', function ($scope, $http) {

  $scope.dropdown = 'Phone Number';

  $scope.setDropdown = function (value) {
    $scope.dropdown = value;
  }
  
  $scope.signUp = function () {

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
